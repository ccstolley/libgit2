/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "remote.h"

#include "git2/config.h"
#include "git2/types.h"
#include "git2/oid.h"
#include "git2/net.h"

#include "config.h"
#include "repository.h"
#include "fetch.h"
#include "refs.h"
#include "refspec.h"
#include "fetchhead.h"
#include "push.h"

#define CONFIG_URL_FMT "remote.%s.url"
#define CONFIG_PUSHURL_FMT "remote.%s.pushurl"
#define CONFIG_FETCH_FMT "remote.%s.fetch"
#define CONFIG_PUSH_FMT "remote.%s.push"
#define CONFIG_TAGOPT_FMT "remote.%s.tagopt"

static int dwim_refspecs(git_vector *out, git_vector *refspecs, git_vector *refs);
static int lookup_remote_prune_config(git_remote *remote, git_config *config, const char *name);
char *apply_insteadof(git_config *config, const char *url, int direction);

static int add_refspec_to(git_vector *vector, const char *string, bool is_fetch)
{
	git_refspec *spec;

	spec = git__calloc(1, sizeof(git_refspec));
	GIT_ERROR_CHECK_ALLOC(spec);

	if (git_refspec__parse(spec, string, is_fetch) < 0) {
		git__free(spec);
		return -1;
	}

	spec->push = !is_fetch;
	if (git_vector_insert(vector, spec) < 0) {
		git_refspec__dispose(spec);
		git__free(spec);
		return -1;
	}

	return 0;
}

static int add_refspec(git_remote *remote, const char *string, bool is_fetch)
{
	return add_refspec_to(&remote->refspecs, string, is_fetch);
}

static int download_tags_value(git_remote *remote, git_config *cfg)
{
	git_config_entry *ce;
	git_buf buf = GIT_BUF_INIT;
	int error;

	if (git_buf_printf(&buf, "remote.%s.tagopt", remote->name) < 0)
		return -1;

	error = git_config__lookup_entry(&ce, cfg, git_buf_cstr(&buf), false);
	git_buf_dispose(&buf);

	if (!error && ce && ce->value) {
		if (!strcmp(ce->value, "--no-tags"))
			remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
		else if (!strcmp(ce->value, "--tags"))
			remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_ALL;
	}

	git_config_entry_free(ce);
	return error;
}

static int ensure_remote_name_is_valid(const char *name)
{
	int valid, error;

	error = git_remote_name_is_valid(&valid, name);

	if (!error && !valid) {
		git_error_set(
			GIT_ERROR_CONFIG,
			"'%s' is not a valid remote name.", name ? name : "(null)");
		error = GIT_EINVALIDSPEC;
	}

	return error;
}

static int write_add_refspec(git_repository *repo, const char *name, const char *refspec, bool fetch)
{
	git_config *cfg;
	git_buf var = GIT_BUF_INIT;
	git_refspec spec;
	const char *fmt;
	int error;

	if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
	    return error;

	fmt = fetch ? CONFIG_FETCH_FMT : CONFIG_PUSH_FMT;

	if ((error = ensure_remote_name_is_valid(name)) < 0)
		return error;

	if ((error = git_refspec__parse(&spec, refspec, fetch)) < 0)
		return error;

	git_refspec__dispose(&spec);

	if ((error = git_buf_printf(&var, fmt, name)) < 0)
		return error;

	/*
	 * "$^" is an unmatchable regexp: it will not match anything at all, so
	 * all values will be considered new and we will not replace any
	 * present value.
	 */
	if ((error = git_config_set_multivar(cfg, var.ptr, "$^", refspec)) < 0) {
		goto cleanup;
	}

cleanup:
	git_buf_dispose(&var);
	return 0;
}

static int canonicalize_url(git_buf *out, const char *in)
{
	if (in == NULL || strlen(in) == 0) {
		git_error_set(GIT_ERROR_INVALID, "cannot set empty URL");
		return GIT_EINVALIDSPEC;
	}

#ifdef GIT_WIN32
	/* Given a UNC path like \\server\path, we need to convert this
	 * to //server/path for compatibility with core git.
	 */
	if (in[0] == '\\' && in[1] == '\\' &&
		(git__isalpha(in[2]) || git__isdigit(in[2]))) {
		const char *c;
		for (c = in; *c; c++)
			git_buf_putc(out, *c == '\\' ? '/' : *c);

		return git_buf_oom(out) ? -1 : 0;
	}
#endif

	return git_buf_puts(out, in);
}

static int default_fetchspec_for_name(git_buf *buf, const char *name)
{
	if (git_buf_printf(buf, "+refs/heads/*:refs/remotes/%s/*", name) < 0)
		return -1;

	return 0;
}

static int ensure_remote_doesnot_exist(git_repository *repo, const char *name)
{
	int error;
	git_remote *remote;

	error = git_remote_lookup(&remote, repo, name);

	if (error == GIT_ENOTFOUND)
		return 0;

	if (error < 0)
		return error;

	git_remote_free(remote);

	git_error_set(GIT_ERROR_CONFIG, "remote '%s' already exists", name);

	return GIT_EEXISTS;
}

int git_remote_create_options_init(git_remote_create_options *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_remote_create_options, GIT_REMOTE_CREATE_OPTIONS_INIT);
	return 0;
}

#ifndef GIT_DEPRECATE_HARD
int git_remote_create_init_options(git_remote_create_options *opts, unsigned int version)
{
	return git_remote_create_options_init(opts, version);
}
#endif

int git_remote_create_with_opts(git_remote **out, const char *url, const git_remote_create_options *opts)
{
	git_remote *remote = NULL;
	git_config *config_ro = NULL, *config_rw;
	git_buf canonical_url = GIT_BUF_INIT;
	git_buf var = GIT_BUF_INIT;
	git_buf specbuf = GIT_BUF_INIT;
	const git_remote_create_options dummy_opts = GIT_REMOTE_CREATE_OPTIONS_INIT;
	int error = -1;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(url);

	if (!opts) {
		opts = &dummy_opts;
	}

	GIT_ERROR_CHECK_VERSION(opts, GIT_REMOTE_CREATE_OPTIONS_VERSION, "git_remote_create_options");

	if (opts->name != NULL) {
		if ((error = ensure_remote_name_is_valid(opts->name)) < 0)
			return error;

		if (opts->repository &&
		    (error = ensure_remote_doesnot_exist(opts->repository, opts->name)) < 0)
			return error;
	}

	if (opts->repository) {
		if ((error = git_repository_config_snapshot(&config_ro, opts->repository)) < 0)
			goto on_error;
	}

	remote = git__calloc(1, sizeof(git_remote));
	GIT_ERROR_CHECK_ALLOC(remote);

	remote->repo = opts->repository;

	if ((error = git_vector_init(&remote->refs, 8, NULL)) < 0 ||
		(error = canonicalize_url(&canonical_url, url)) < 0)
		goto on_error;

	if (opts->repository && !(opts->flags & GIT_REMOTE_CREATE_SKIP_INSTEADOF)) {
		remote->url = apply_insteadof(config_ro, canonical_url.ptr, GIT_DIRECTION_FETCH);
	} else {
		remote->url = git__strdup(canonical_url.ptr);
	}
	GIT_ERROR_CHECK_ALLOC(remote->url);

	if (opts->name != NULL) {
		remote->name = git__strdup(opts->name);
		GIT_ERROR_CHECK_ALLOC(remote->name);

		if (opts->repository &&
		    ((error = git_buf_printf(&var, CONFIG_URL_FMT, opts->name)) < 0 ||
		    (error = git_repository_config__weakptr(&config_rw, opts->repository)) < 0 ||
		    (error = git_config_set_string(config_rw, var.ptr, canonical_url.ptr)) < 0))
			goto on_error;
	}

	if (opts->fetchspec != NULL ||
	    (opts->name && !(opts->flags & GIT_REMOTE_CREATE_SKIP_DEFAULT_FETCHSPEC))) {
		const char *fetch = NULL;
		if (opts->fetchspec) {
			fetch = opts->fetchspec;
		} else {
			if ((error = default_fetchspec_for_name(&specbuf, opts->name)) < 0)
				goto on_error;

			fetch = git_buf_cstr(&specbuf);
		}

		if ((error = add_refspec(remote, fetch, true)) < 0)
			goto on_error;

		/* only write for named remotes with a repository */
		if (opts->repository && opts->name &&
		    ((error = write_add_refspec(opts->repository, opts->name, fetch, true)) < 0 ||
		    (error = lookup_remote_prune_config(remote, config_ro, opts->name)) < 0))
			goto on_error;

		/* Move the data over to where the matching functions can find them */
		if ((error = dwim_refspecs(&remote->active_refspecs, &remote->refspecs, &remote->refs)) < 0)
			goto on_error;
	}

	/* A remote without a name doesn't download tags */
	if (!opts->name)
		remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
	else
		remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_AUTO;


	git_buf_dispose(&var);

	*out = remote;
	error = 0;

on_error:
	if (error)
		git_remote_free(remote);

	git_config_free(config_ro);
	git_buf_dispose(&specbuf);
	git_buf_dispose(&canonical_url);
	git_buf_dispose(&var);
	return error;
}

int git_remote_create(git_remote **out, git_repository *repo, const char *name, const char *url)
{
	git_buf buf = GIT_BUF_INIT;
	int error;
	git_remote_create_options opts = GIT_REMOTE_CREATE_OPTIONS_INIT;

	/* Those 2 tests are duplicated here because of backward-compatibility */
	if ((error = ensure_remote_name_is_valid(name)) < 0)
		return error;

	if (canonicalize_url(&buf, url) < 0)
		return GIT_ERROR;

	git_buf_clear(&buf);

	opts.repository = repo;
	opts.name = name;

	error = git_remote_create_with_opts(out, url, &opts);

	git_buf_dispose(&buf);

	return error;
}

int git_remote_create_with_fetchspec(git_remote **out, git_repository *repo, const char *name, const char *url, const char *fetch)
{
	int error;
	git_remote_create_options opts = GIT_REMOTE_CREATE_OPTIONS_INIT;

	if ((error = ensure_remote_name_is_valid(name)) < 0)
		return error;

	opts.repository = repo;
	opts.name = name;
	opts.fetchspec = fetch;
	opts.flags = GIT_REMOTE_CREATE_SKIP_DEFAULT_FETCHSPEC;

	return git_remote_create_with_opts(out, url, &opts);
}

int git_remote_create_anonymous(git_remote **out, git_repository *repo, const char *url)
{
	git_remote_create_options opts = GIT_REMOTE_CREATE_OPTIONS_INIT;

	opts.repository = repo;

	return git_remote_create_with_opts(out, url, &opts);
}

int git_remote_create_detached(git_remote **out, const char *url)
{
	return git_remote_create_with_opts(out, url, NULL);
}

int git_remote_dup(git_remote **dest, git_remote *source)
{
	size_t i;
	int error = 0;
	git_refspec *spec;
	git_remote *remote = git__calloc(1, sizeof(git_remote));
	GIT_ERROR_CHECK_ALLOC(remote);

	if (source->name != NULL) {
		remote->name = git__strdup(source->name);
		GIT_ERROR_CHECK_ALLOC(remote->name);
	}

	if (source->url != NULL) {
		remote->url = git__strdup(source->url);
		GIT_ERROR_CHECK_ALLOC(remote->url);
	}

	if (source->pushurl != NULL) {
		remote->pushurl = git__strdup(source->pushurl);
		GIT_ERROR_CHECK_ALLOC(remote->pushurl);
	}

	remote->repo = source->repo;
	remote->download_tags = source->download_tags;
	remote->prune_refs = source->prune_refs;

	if (git_vector_init(&remote->refs, 32, NULL) < 0 ||
	    git_vector_init(&remote->refspecs, 2, NULL) < 0 ||
	    git_vector_init(&remote->active_refspecs, 2, NULL) < 0) {
		error = -1;
		goto cleanup;
	}

	git_vector_foreach(&source->refspecs, i, spec) {
		if ((error = add_refspec(remote, spec->string, !spec->push)) < 0)
			goto cleanup;
	}

	*dest = remote;

cleanup:

	if (error < 0)
		git__free(remote);

	return error;
}

struct refspec_cb_data {
	git_remote *remote;
	int fetch;
};

static int refspec_cb(const git_config_entry *entry, void *payload)
{
	struct refspec_cb_data *data = (struct refspec_cb_data *)payload;
	return add_refspec(data->remote, entry->value, data->fetch);
}

static int get_optional_config(
	bool *found, git_config *config, git_buf *buf,
	git_config_foreach_cb cb, void *payload)
{
	int error = 0;
	const char *key = git_buf_cstr(buf);

	if (git_buf_oom(buf))
		return -1;

	if (cb != NULL)
		error = git_config_get_multivar_foreach(config, key, NULL, cb, payload);
	else
		error = git_config_get_string(payload, config, key);

	if (found)
		*found = !error;

	if (error == GIT_ENOTFOUND) {
		git_error_clear();
		error = 0;
	}

	return error;
}

int git_remote_lookup(git_remote **out, git_repository *repo, const char *name)
{
	git_remote *remote = NULL;
	git_buf buf = GIT_BUF_INIT;
	const char *val;
	int error = 0;
	git_config *config;
	struct refspec_cb_data data = { NULL };
	bool optional_setting_found = false, found;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);

	if ((error = ensure_remote_name_is_valid(name)) < 0)
		return error;

	if ((error = git_repository_config_snapshot(&config, repo)) < 0)
		return error;

	remote = git__calloc(1, sizeof(git_remote));
	GIT_ERROR_CHECK_ALLOC(remote);

	remote->name = git__strdup(name);
	GIT_ERROR_CHECK_ALLOC(remote->name);

	if (git_vector_init(&remote->refs, 32, NULL) < 0 ||
	    git_vector_init(&remote->refspecs, 2, NULL) < 0 ||
	    git_vector_init(&remote->passive_refspecs, 2, NULL) < 0 ||
	    git_vector_init(&remote->active_refspecs, 2, NULL) < 0) {
		error = -1;
		goto cleanup;
	}

	if ((error = git_buf_printf(&buf, "remote.%s.url", name)) < 0)
		goto cleanup;

	if ((error = get_optional_config(&found, config, &buf, NULL, (void *)&val)) < 0)
		goto cleanup;

	optional_setting_found |= found;

	remote->repo = repo;
	remote->download_tags = GIT_REMOTE_DOWNLOAD_TAGS_AUTO;

	if (found && strlen(val) > 0) {
		remote->url = apply_insteadof(config, val, GIT_DIRECTION_FETCH);
		GIT_ERROR_CHECK_ALLOC(remote->url);
	}

	val = NULL;
	git_buf_clear(&buf);
	git_buf_printf(&buf, "remote.%s.pushurl", name);

	if ((error = get_optional_config(&found, config, &buf, NULL, (void *)&val)) < 0)
		goto cleanup;

	optional_setting_found |= found;

	if (!optional_setting_found) {
		error = GIT_ENOTFOUND;
		git_error_set(GIT_ERROR_CONFIG, "remote '%s' does not exist", name);
		goto cleanup;
	}

	if (found && strlen(val) > 0) {
		remote->pushurl = apply_insteadof(config, val, GIT_DIRECTION_PUSH);
		GIT_ERROR_CHECK_ALLOC(remote->pushurl);
	}

	data.remote = remote;
	data.fetch = true;

	git_buf_clear(&buf);
	git_buf_printf(&buf, "remote.%s.fetch", name);

	if ((error = get_optional_config(NULL, config, &buf, refspec_cb, &data)) < 0)
		goto cleanup;

	data.fetch = false;
	git_buf_clear(&buf);
	git_buf_printf(&buf, "remote.%s.push", name);

	if ((error = get_optional_config(NULL, config, &buf, refspec_cb, &data)) < 0)
		goto cleanup;

	if ((error = download_tags_value(remote, config)) < 0)
		goto cleanup;

	if ((error = lookup_remote_prune_config(remote, config, name)) < 0)
		goto cleanup;

	/* Move the data over to where the matching functions can find them */
	if ((error = dwim_refspecs(&remote->active_refspecs, &remote->refspecs, &remote->refs)) < 0)
		goto cleanup;

	*out = remote;

cleanup:
	git_config_free(config);
	git_buf_dispose(&buf);

	if (error < 0)
		git_remote_free(remote);

	return error;
}

static int lookup_remote_prune_config(git_remote *remote, git_config *config, const char *name)
{
	git_buf buf = GIT_BUF_INIT;
	int error = 0;

	git_buf_printf(&buf, "remote.%s.prune", name);

	if ((error = git_config_get_bool(&remote->prune_refs, config, git_buf_cstr(&buf))) < 0) {
		if (error == GIT_ENOTFOUND) {
			git_error_clear();

			if ((error = git_config_get_bool(&remote->prune_refs, config, "fetch.prune")) < 0) {
				if (error == GIT_ENOTFOUND) {
					git_error_clear();
					error = 0;
				}
			}
		}
	}

	git_buf_dispose(&buf);
	return error;
}

const char *git_remote_name(const git_remote *remote)
{
	GIT_ASSERT_ARG_WITH_RETVAL(remote, NULL);
	return remote->name;
}

git_repository *git_remote_owner(const git_remote *remote)
{
	GIT_ASSERT_ARG_WITH_RETVAL(remote, NULL);
	return remote->repo;
}

const char *git_remote_url(const git_remote *remote)
{
	GIT_ASSERT_ARG_WITH_RETVAL(remote, NULL);
	return remote->url;
}

int git_remote_set_instance_url(git_remote *remote, const char *url)
{
	char *tmp;

	GIT_ASSERT_ARG(remote);
	GIT_ASSERT_ARG(url);

	if ((tmp = git__strdup(url)) == NULL)
		return -1;

	git__free(remote->url);
	remote->url = tmp;

	return 0;
}

static int set_url(git_repository *repo, const char *remote, const char *pattern, const char *url)
{
	git_config *cfg;
	git_buf buf = GIT_BUF_INIT, canonical_url = GIT_BUF_INIT;
	int error;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(remote);

	if ((error = ensure_remote_name_is_valid(remote)) < 0)
		return error;

	if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
		return error;

	if ((error = git_buf_printf(&buf, pattern, remote)) < 0)
		return error;

	if (url) {
		if ((error = canonicalize_url(&canonical_url, url)) < 0)
			goto cleanup;

		error = git_config_set_string(cfg, buf.ptr, url);
	} else {
		error = git_config_delete_entry(cfg, buf.ptr);
	}

cleanup:
	git_buf_dispose(&canonical_url);
	git_buf_dispose(&buf);

	return error;
}

int git_remote_set_url(git_repository *repo, const char *remote, const char *url)
{
	return set_url(repo, remote, CONFIG_URL_FMT, url);
}

const char *git_remote_pushurl(const git_remote *remote)
{
	GIT_ASSERT_ARG_WITH_RETVAL(remote, NULL);
	return remote->pushurl;
}

int git_remote_set_instance_pushurl(git_remote *remote, const char *url)
{
	char *tmp;

	GIT_ASSERT_ARG(remote);
	GIT_ASSERT_ARG(url);

	if ((tmp = git__strdup(url)) == NULL)
		return -1;

	git__free(remote->pushurl);
	remote->pushurl = tmp;

	return 0;
}

int git_remote_set_pushurl(git_repository *repo, const char *remote, const char *url)
{
	return set_url(repo, remote, CONFIG_PUSHURL_FMT, url);
}

static int resolve_url(
	git_buf *resolved_url,
	const char *url,
	int direction,
	const git_remote_callbacks *callbacks)
{
#ifdef GIT_DEPRECATE_HARD
	GIT_UNUSED(direction);
	GIT_UNUSED(callbacks);
#else
	int status, error;

	if (callbacks && callbacks->resolve_url) {
		git_buf_clear(resolved_url);
		status = callbacks->resolve_url(resolved_url, url, direction, callbacks->payload);
		if (status != GIT_PASSTHROUGH) {
			git_error_set_after_callback_function(status, "git_resolve_url_cb");

			if ((error = git_buf_sanitize(resolved_url)) < 0)
				return error;

			return status;
		}
	}
#endif

	return git_buf_sets(resolved_url, url);
}

int git_remote__urlfordirection(
	git_buf *url_out,
	struct git_remote *remote,
	int direction,
	const git_remote_callbacks *callbacks)
{
	const char *url = NULL;

	GIT_ASSERT_ARG(remote);
	GIT_ASSERT_ARG(direction == GIT_DIRECTION_FETCH || direction == GIT_DIRECTION_PUSH);

	if (callbacks && callbacks->remote_ready) {
		int status = callbacks->remote_ready(remote, direction, callbacks->payload);

		if (status != 0 && status != GIT_PASSTHROUGH) {
			git_error_set_after_callback_function(status, "git_remote_ready_cb");
			return status;
		}
	}

	if (direction == GIT_DIRECTION_FETCH)
		url = remote->url;
	else if (direction == GIT_DIRECTION_PUSH)
		url = remote->pushurl ? remote->pushurl : remote->url;

	if (!url) {
		git_error_set(GIT_ERROR_INVALID,
			"malformed remote '%s' - missing %s URL",
			remote->name ? remote->name : "(anonymous)",
			direction == GIT_DIRECTION_FETCH ? "fetch" : "push");
		return GIT_EINVALID;
	}

	return resolve_url(url_out, url, direction, callbacks);
}

static int remote_transport_set_callbacks(git_transport *t, const git_remote_callbacks *cbs)
{
	if (!t->set_callbacks || !cbs)
		return 0;

	return t->set_callbacks(t, cbs->sideband_progress, NULL,
				cbs->certificate_check, cbs->payload);
}

static int set_transport_custom_headers(git_transport *t, const git_strarray *custom_headers)
{
	if (!t->set_custom_headers)
		return 0;

	return t->set_custom_headers(t, custom_headers);
}

int git_remote__connect(git_remote *remote, git_direction direction, const git_remote_callbacks *callbacks, const git_remote_connection_opts *conn)
{
	git_transport *t;
	git_buf url = GIT_BUF_INIT;
	int flags = GIT_TRANSPORTFLAGS_NONE;
	int error;
	void *payload = NULL;
	git_credential_acquire_cb credentials = NULL;
	git_transport_cb transport = NULL;

	GIT_ASSERT_ARG(remote);

	if (callbacks) {
		GIT_ERROR_CHECK_VERSION(callbacks, GIT_REMOTE_CALLBACKS_VERSION, "git_remote_callbacks");
		credentials = callbacks->credentials;
		transport   = callbacks->transport;
		payload     = callbacks->payload;
	}

	if (conn->proxy)
		GIT_ERROR_CHECK_VERSION(conn->proxy, GIT_PROXY_OPTIONS_VERSION, "git_proxy_options");

	t = remote->transport;

	if ((error = git_remote__urlfordirection(&url, remote, direction, callbacks)) < 0)
		goto on_error;

	/* If we don't have a transport object yet, and the caller specified a
	 * custom transport factory, use that */
	if (!t && transport &&
		(error = transport(&t, remote, payload)) < 0)
		goto on_error;

	/* If we still don't have a transport, then use the global
	 * transport registrations which map URI schemes to transport factories */
	if (!t && (error = git_transport_new(&t, remote, url.ptr)) < 0)
		goto on_error;

	if ((error = set_transport_custom_headers(t, conn->custom_headers)) != 0)
		goto on_error;

	if ((error = remote_transport_set_callbacks(t, callbacks)) < 0 ||
	    (error = t->connect(t, url.ptr, credentials, payload, conn->proxy, direction, flags)) != 0)
		goto on_error;

	remote->transport = t;

	git_buf_dispose(&url);

	return 0;

on_error:
	if (t)
		t->free(t);

	git_buf_dispose(&url);

	if (t == remote->transport)
		remote->transport = NULL;

	return error;
}

int git_remote_connect(git_remote *remote, git_direction direction, const git_remote_callbacks *callbacks, const git_proxy_options *proxy, const git_strarray *custom_headers)
{
	git_remote_connection_opts conn;

	conn.proxy = proxy;
	conn.custom_headers = custom_headers;

	return git_remote__connect(remote, direction, callbacks, &conn);
}

int git_remote_ls(const git_remote_head ***out, size_t *size, git_remote *remote)
{
	GIT_ASSERT_ARG(remote);

	if (!remote->transport) {
		git_error_set(GIT_ERROR_NET, "this remote has never connected");
		return -1;
	}

	return remote->transport->ls(out, size, remote->transport);
}

static int lookup_config(char **out, git_config *cfg, const char *name)
{
	git_config_entry *ce = NULL;
	int error;

	if ((error = git_config__lookup_entry(&ce, cfg, name, false)) < 0)
		return error;

	if (ce && ce->value) {
		*out = git__strdup(ce->value);
		GIT_ERROR_CHECK_ALLOC(*out);
	} else {
		error = GIT_ENOTFOUND;
	}

	git_config_entry_free(ce);
	return error;
}

static void url_config_trim(git_net_url *url)
{
	size_t len = strlen(url->path);

	if (url->path[len - 1] == '/') {
		len--;
	} else {
		while (len && url->path[len - 1] != '/')
			len--;
	}

	url->path[len] = '\0';
}

static int http_proxy_config(char **out, git_remote *remote, git_net_url *url)
{
	git_config *cfg;
	git_buf buf = GIT_BUF_INIT;
	git_net_url lookup_url = GIT_NET_URL_INIT;
	int error;

	if ((error = git_net_url_dup(&lookup_url, url)) < 0 ||
	    (error = git_repository_config__weakptr(&cfg, remote->repo)) < 0)
		goto done;

	/* remote.<name>.proxy config setting */
	if (remote->name && remote->name[0]) {
		git_buf_clear(&buf);

		if ((error = git_buf_printf(&buf, "remote.%s.proxy", remote->name)) < 0 ||
		    (error = lookup_config(out, cfg, buf.ptr)) != GIT_ENOTFOUND)
			goto done;
	}

	while (true) {
		git_buf_clear(&buf);

		if ((error = git_buf_puts(&buf, "http.")) < 0 ||
		    (error = git_net_url_fmt(&buf, &lookup_url)) < 0 ||
		    (error = git_buf_puts(&buf, ".proxy")) < 0 ||
		    (error = lookup_config(out, cfg, buf.ptr)) != GIT_ENOTFOUND)
			goto done;

		if (! lookup_url.path[0])
			break;

		url_config_trim(&lookup_url);
	}

	git_buf_clear(&buf);

	error = lookup_config(out, cfg, "http.proxy");

done:
	git_buf_dispose(&buf);
	git_net_url_dispose(&lookup_url);
	return error;
}

static int http_proxy_env(char **out, git_remote *remote, git_net_url *url)
{
	git_buf proxy_env = GIT_BUF_INIT, no_proxy_env = GIT_BUF_INIT;
	bool use_ssl = (strcmp(url->scheme, "https") == 0);
	int error;

	GIT_UNUSED(remote);

	/* http_proxy / https_proxy environment variables */
	error = git__getenv(&proxy_env, use_ssl ? "https_proxy" : "http_proxy");

	/* try uppercase environment variables */
	if (error == GIT_ENOTFOUND)
		error = git__getenv(&proxy_env, use_ssl ? "HTTPS_PROXY" : "HTTP_PROXY");

	if (error)
		goto done;

	/* no_proxy/NO_PROXY environment variables */
	error = git__getenv(&no_proxy_env, "no_proxy");

	if (error == GIT_ENOTFOUND)
		error = git__getenv(&no_proxy_env, "NO_PROXY");

	if (error && error != GIT_ENOTFOUND)
		goto done;

	if (!git_net_url_matches_pattern_list(url, no_proxy_env.ptr))
		*out = git_buf_detach(&proxy_env);
	else
		error = GIT_ENOTFOUND;

done:
	git_buf_dispose(&proxy_env);
	git_buf_dispose(&no_proxy_env);
	return error;
}

int git_remote__http_proxy(char **out, git_remote *remote, git_net_url *url)
{
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(remote);
	GIT_ASSERT_ARG(remote->repo);

	*out = NULL;

	/*
	 * Go through the possible sources for proxy configuration,
	 * Examine the various git config options first, then
	 * consult environment variables.
	 */
	if ((error = http_proxy_config(out, remote, url)) != GIT_ENOTFOUND ||
	    (error = http_proxy_env(out, remote, url)) != GIT_ENOTFOUND)
		return error;

	return 0;
}

/* DWIM `refspecs` based on `refs` and append the output to `out` */
static int dwim_refspecs(git_vector *out, git_vector *refspecs, git_vector *refs)
{
	size_t i;
	git_refspec *spec;

	git_vector_foreach(refspecs, i, spec) {
		if (git_refspec__dwim_one(out, spec, refs) < 0)
			return -1;
	}

	return 0;
}

static void free_refspecs(git_vector *vec)
{
	size_t i;
	git_refspec *spec;

	git_vector_foreach(vec, i, spec) {
		git_refspec__dispose(spec);
		git__free(spec);
	}

	git_vector_clear(vec);
}

static int remote_head_cmp(const void *_a, const void *_b)
{
	const git_remote_head *a = (git_remote_head *) _a;
	const git_remote_head *b = (git_remote_head *) _b;

	return git__strcmp_cb(a->name, b->name);
}

static int ls_to_vector(git_vector *out, git_remote *remote)
{
	git_remote_head **heads;
	size_t heads_len, i;

	if (git_remote_ls((const git_remote_head ***)&heads, &heads_len, remote) < 0)
		return -1;

	if (git_vector_init(out, heads_len, remote_head_cmp) < 0)
		return -1;

	for (i = 0; i < heads_len; i++) {
		if (git_vector_insert(out, heads[i]) < 0)
			return -1;
	}

	return 0;
}

int git_remote_download(git_remote *remote, const git_strarray *refspecs, const git_fetch_options *opts)
{
	int error = -1;
	size_t i;
	git_vector *to_active, specs = GIT_VECTOR_INIT, refs = GIT_VECTOR_INIT;
	const git_remote_callbacks *cbs = NULL;
	const git_strarray *custom_headers = NULL;
	const git_proxy_options *proxy = NULL;

	GIT_ASSERT_ARG(remote);

	if (!remote->repo) {
		git_error_set(GIT_ERROR_INVALID, "cannot download detached remote");
		return -1;
	}

	if (opts) {
		GIT_ERROR_CHECK_VERSION(&opts->callbacks, GIT_REMOTE_CALLBACKS_VERSION, "git_remote_callbacks");
		cbs = &opts->callbacks;
		custom_headers = &opts->custom_headers;
		GIT_ERROR_CHECK_VERSION(&opts->proxy_opts, GIT_PROXY_OPTIONS_VERSION, "git_proxy_options");
		proxy = &opts->proxy_opts;
	}

	if (!git_remote_connected(remote) &&
	    (error = git_remote_connect(remote, GIT_DIRECTION_FETCH, cbs, proxy, custom_headers)) < 0)
		goto on_error;

	if (ls_to_vector(&refs, remote) < 0)
		return -1;

	if ((git_vector_init(&specs, 0, NULL)) < 0)
		goto on_error;

	remote->passed_refspecs = 0;
	if (!refspecs || !refspecs->count) {
		to_active = &remote->refspecs;
	} else {
		for (i = 0; i < refspecs->count; i++) {
			if ((error = add_refspec_to(&specs, refspecs->strings[i], true)) < 0)
				goto on_error;
		}

		to_active = &specs;
		remote->passed_refspecs = 1;
	}

	free_refspecs(&remote->passive_refspecs);
	if ((error = dwim_refspecs(&remote->passive_refspecs, &remote->refspecs, &refs)) < 0)
		goto on_error;

	free_refspecs(&remote->active_refspecs);
	error = dwim_refspecs(&remote->active_refspecs, to_active, &refs);

	git_vector_free(&refs);
	free_refspecs(&specs);
	git_vector_free(&specs);

	if (error < 0)
		return error;

	if (remote->push) {
		git_push_free(remote->push);
		remote->push = NULL;
	}

	if ((error = git_fetch_negotiate(remote, opts)) < 0)
		return error;

	return git_fetch_download_pack(remote, cbs);

on_error:
	git_vector_free(&refs);
	free_refspecs(&specs);
	git_vector_free(&specs);
	return error;
}

int git_remote_fetch(
		git_remote *remote,
		const git_strarray *refspecs,
		const git_fetch_options *opts,
		const char *reflog_message)
{
	int error, update_fetchhead = 1;
	git_remote_autotag_option_t tagopt = remote->download_tags;
	bool prune = false;
	git_buf reflog_msg_buf = GIT_BUF_INIT;
	const git_remote_callbacks *cbs = NULL;
	git_remote_connection_opts conn = GIT_REMOTE_CONNECTION_OPTIONS_INIT;

	if (opts) {
		GIT_ERROR_CHECK_VERSION(&opts->callbacks, GIT_REMOTE_CALLBACKS_VERSION, "git_remote_callbacks");
		cbs = &opts->callbacks;
		conn.custom_headers = &opts->custom_headers;
		update_fetchhead = opts->update_fetchhead;
		tagopt = opts->download_tags;
		GIT_ERROR_CHECK_VERSION(&opts->proxy_opts, GIT_PROXY_OPTIONS_VERSION, "git_proxy_options");
		conn.proxy = &opts->proxy_opts;
	}

	/* Connect and download everything */
	if ((error = git_remote__connect(remote, GIT_DIRECTION_FETCH, cbs, &conn)) != 0)
		return error;

	error = git_remote_download(remote, refspecs, opts);

	/* We don't need to be connected anymore */
	git_remote_disconnect(remote);

	/* If the download failed, return the error */
	if (error != 0)
		return error;

	/* Default reflog message */
	if (reflog_message)
		git_buf_sets(&reflog_msg_buf, reflog_message);
	else {
		git_buf_printf(&reflog_msg_buf, "fetch %s",
				remote->name ? remote->name : remote->url);
	}

	/* Create "remote/foo" branches for all remote branches */
	error = git_remote_update_tips(remote, cbs, update_fetchhead, tagopt, git_buf_cstr(&reflog_msg_buf));
	git_buf_dispose(&reflog_msg_buf);
	if (error < 0)
		return error;

	if (opts && opts->prune == GIT_FETCH_PRUNE)
		prune = true;
	else if (opts && opts->prune == GIT_FETCH_PRUNE_UNSPECIFIED && remote->prune_refs)
		prune = true;
	else if (opts && opts->prune == GIT_FETCH_NO_PRUNE)
		prune = false;
	else
		prune = remote->prune_refs;

	if (prune)
		error = git_remote_prune(remote, cbs);

	return error;
}

static int remote_head_for_fetchspec_src(git_remote_head **out, git_vector *update_heads, const char *fetchspec_src)
{
	unsigned int i;
	git_remote_head *remote_ref;

	GIT_ASSERT_ARG(update_heads);
	GIT_ASSERT_ARG(fetchspec_src);

	*out = NULL;

	git_vector_foreach(update_heads, i, remote_ref) {
		if (strcmp(remote_ref->name, fetchspec_src) == 0) {
			*out = remote_ref;
			break;
		}
	}

	return 0;
}

static int ref_to_update(int *update, git_buf *remote_name, git_remote *remote, git_refspec *spec, const char *ref_name)
{
	int error = 0;
	git_repository *repo;
	git_buf upstream_remote = GIT_BUF_INIT;
	git_buf upstream_name = GIT_BUF_INIT;

	repo = git_remote_owner(remote);

	if ((!git_reference__is_branch(ref_name)) ||
	    !git_remote_name(remote) ||
	    (error = git_branch_upstream_remote(&upstream_remote, repo, ref_name) < 0) ||
	    git__strcmp(git_remote_name(remote), git_buf_cstr(&upstream_remote)) ||
	    (error = git_branch_upstream_name(&upstream_name, repo, ref_name)) < 0 ||
	    !git_refspec_dst_matches(spec, git_buf_cstr(&upstream_name)) ||
	    (error = git_refspec_rtransform(remote_name, spec, upstream_name.ptr)) < 0) {
		/* Not an error if there is no upstream */
		if (error == GIT_ENOTFOUND) {
			git_error_clear();
			error = 0;
		}

		*update = 0;
	} else {
		*update = 1;
	}

	git_buf_dispose(&upstream_remote);
	git_buf_dispose(&upstream_name);
	return error;
}

static int remote_head_for_ref(git_remote_head **out, git_remote *remote, git_refspec *spec, git_vector *update_heads, git_reference *ref)
{
	git_reference *resolved_ref = NULL;
	git_buf remote_name = GIT_BUF_INIT;
	git_config *config = NULL;
	const char *ref_name;
	int error = 0, update;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(spec);
	GIT_ASSERT_ARG(ref);

	*out = NULL;

	error = git_reference_resolve(&resolved_ref, ref);

	/* If we're in an unborn branch, let's pretend nothing happened */
	if (error == GIT_ENOTFOUND && git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
		ref_name = git_reference_symbolic_target(ref);
		error = 0;
	} else {
		ref_name = git_reference_name(resolved_ref);
	}

	/*
	 * The ref name may be unresolvable - perhaps it's pointing to
	 * something invalid.  In this case, there is no remote head for
	 * this ref.
	 */
	if (!ref_name) {
		error = 0;
		goto cleanup;
	}

	if ((error = ref_to_update(&update, &remote_name, remote, spec, ref_name)) < 0)
		goto cleanup;

	if (update)
		error = remote_head_for_fetchspec_src(out, update_heads, git_buf_cstr(&remote_name));

cleanup:
	git_buf_dispose(&remote_name);
	git_reference_free(resolved_ref);
	git_config_free(config);
	return error;
}

static int git_remote_write_fetchhead(git_remote *remote, git_refspec *spec, git_vector *update_heads)
{
	git_reference *head_ref = NULL;
	git_fetchhead_ref *fetchhead_ref;
	git_remote_head *remote_ref, *merge_remote_ref;
	git_vector fetchhead_refs;
	bool include_all_fetchheads;
	unsigned int i = 0;
	int error = 0;

	GIT_ASSERT_ARG(remote);

	/* no heads, nothing to do */
	if (update_heads->length == 0)
		return 0;

	if (git_vector_init(&fetchhead_refs, update_heads->length, git_fetchhead_ref_cmp) < 0)
		return -1;

	/* Iff refspec is * (but not subdir slash star), include tags */
	include_all_fetchheads = (strcmp(GIT_REFS_HEADS_DIR "*", git_refspec_src(spec)) == 0);

	/* Determine what to merge: if refspec was a wildcard, just use HEAD */
	if (git_refspec_is_wildcard(spec)) {
		if ((error = git_reference_lookup(&head_ref, remote->repo, GIT_HEAD_FILE)) < 0 ||
			(error = remote_head_for_ref(&merge_remote_ref, remote, spec, update_heads, head_ref)) < 0)
				goto cleanup;
	} else {
		/* If we're fetching a single refspec, that's the only thing that should be in FETCH_HEAD. */
		if ((error = remote_head_for_fetchspec_src(&merge_remote_ref, update_heads, git_refspec_src(spec))) < 0)
			goto cleanup;
	}

	/* Create the FETCH_HEAD file */
	git_vector_foreach(update_heads, i, remote_ref) {
		int merge_this_fetchhead = (merge_remote_ref == remote_ref);

		if (!include_all_fetchheads &&
			!git_refspec_src_matches(spec, remote_ref->name) &&
			!merge_this_fetchhead)
			continue;

		if (git_fetchhead_ref_create(&fetchhead_ref,
			&remote_ref->oid,
			merge_this_fetchhead,
			remote_ref->name,
			git_remote_url(remote)) < 0)
			goto cleanup;

		if (git_vector_insert(&fetchhead_refs, fetchhead_ref) < 0)
			goto cleanup;
	}

	git_fetchhead_write(remote->repo, &fetchhead_refs);

cleanup:
	for (i = 0; i < fetchhead_refs.length; ++i)
		git_fetchhead_ref_free(fetchhead_refs.contents[i]);

	git_vector_free(&fetchhead_refs);
	git_reference_free(head_ref);

	return error;
}

/**
 * Generate a list of candidates for pruning by getting a list of
 * references which match the rhs of an active refspec.
 */
static int prune_candidates(git_vector *candidates, git_remote *remote)
{
	git_strarray arr = { 0 };
	size_t i;
	int error;

	if ((error = git_reference_list(&arr, remote->repo)) < 0)
		return error;

	for (i = 0; i < arr.count; i++) {
		const char *refname = arr.strings[i];
		char *refname_dup;

		if (!git_remote__matching_dst_refspec(remote, refname))
			continue;

		refname_dup = git__strdup(refname);
		GIT_ERROR_CHECK_ALLOC(refname_dup);

		if ((error = git_vector_insert(candidates, refname_dup)) < 0)
			goto out;
	}

out:
	git_strarray_dispose(&arr);
	return error;
}

static int find_head(const void *_a, const void *_b)
{
	git_remote_head *a = (git_remote_head *) _a;
	git_remote_head *b = (git_remote_head *) _b;

	return strcmp(a->name, b->name);
}

int git_remote_prune(git_remote *remote, const git_remote_callbacks *callbacks)
{
	size_t i, j;
	git_vector remote_refs = GIT_VECTOR_INIT;
	git_vector candidates = GIT_VECTOR_INIT;
	const git_refspec *spec;
	const char *refname;
	int error;
	git_oid zero_id = {{ 0 }};

	if (callbacks)
		GIT_ERROR_CHECK_VERSION(callbacks, GIT_REMOTE_CALLBACKS_VERSION, "git_remote_callbacks");

	if ((error = ls_to_vector(&remote_refs, remote)) < 0)
		goto cleanup;

	git_vector_set_cmp(&remote_refs, find_head);

	if ((error = prune_candidates(&candidates, remote)) < 0)
		goto cleanup;

	/*
	 * Remove those entries from the candidate list for which we
	 * can find a remote reference in at least one refspec.
	 */
	git_vector_foreach(&candidates, i, refname) {
		git_vector_foreach(&remote->active_refspecs, j, spec) {
			git_buf buf = GIT_BUF_INIT;
			size_t pos;
			char *src_name;
			git_remote_head key = {0};

			if (!git_refspec_dst_matches(spec, refname))
				continue;

			if ((error = git_refspec_rtransform(&buf, spec, refname)) < 0)
				goto cleanup;

			key.name = (char *) git_buf_cstr(&buf);
			error = git_vector_bsearch(&pos, &remote_refs, &key);
			git_buf_dispose(&buf);

			if (error < 0 && error != GIT_ENOTFOUND)
				goto cleanup;

			if (error == GIT_ENOTFOUND)
				continue;

			/* If we did find a source, remove it from the candidates. */
			if ((error = git_vector_set((void **) &src_name, &candidates, i, NULL)) < 0)
				goto cleanup;

			git__free(src_name);
			break;
		}
	}

	/*
	 * For those candidates still left in the list, we need to
	 * remove them. We do not remove symrefs, as those are for
	 * stuff like origin/HEAD which will never match, but we do
	 * not want to remove them.
	 */
	git_vector_foreach(&candidates, i, refname) {
		git_reference *ref;
		git_oid id;

		if (refname == NULL)
			continue;

		error = git_reference_lookup(&ref, remote->repo, refname);
		/* as we want it gone, let's not consider this an error */
		if (error == GIT_ENOTFOUND)
			continue;

		if (error < 0)
			goto cleanup;

		if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
			git_reference_free(ref);
			continue;
		}

		git_oid_cpy(&id, git_reference_target(ref));
		error = git_reference_delete(ref);
		git_reference_free(ref);
		if (error < 0)
			goto cleanup;

		if (callbacks && callbacks->update_tips)
			error = callbacks->update_tips(refname, &id, &zero_id, callbacks->payload);

		if (error < 0)
			goto cleanup;
	}

cleanup:
	git_vector_free(&remote_refs);
	git_vector_free_deep(&candidates);
	return error;
}

static int update_tips_for_spec(
		git_remote *remote,
		const git_remote_callbacks *callbacks,
		int update_fetchhead,
		git_remote_autotag_option_t tagopt,
		git_refspec *spec,
		git_vector *refs,
		const char *log_message)
{
	int error = 0, autotag, valid;
	unsigned int i = 0;
	git_buf refname = GIT_BUF_INIT;
	git_oid old;
	git_odb *odb;
	git_remote_head *head;
	git_reference *ref;
	git_refspec tagspec;
	git_vector update_heads;

	GIT_ASSERT_ARG(remote);

	if (git_repository_odb__weakptr(&odb, remote->repo) < 0)
		return -1;

	if (git_refspec__parse(&tagspec, GIT_REFSPEC_TAGS, true) < 0)
		return -1;

	/* Make a copy of the transport's refs */
	if (git_vector_init(&update_heads, 16, NULL) < 0)
		return -1;

	for (; i < refs->length; ++i) {
		head = git_vector_get(refs, i);
		autotag = 0;
		git_buf_clear(&refname);

		/* Ignore malformed ref names (which also saves us from tag^{} */
		if (git_reference_name_is_valid(&valid, head->name) < 0)
			goto on_error;

		if (!valid)
			continue;

		/* If we have a tag, see if the auto-follow rules say to update it */
		if (git_refspec_src_matches(&tagspec, head->name)) {
			if (tagopt != GIT_REMOTE_DOWNLOAD_TAGS_NONE) {

				if (tagopt == GIT_REMOTE_DOWNLOAD_TAGS_AUTO)
					autotag = 1;

				git_buf_clear(&refname);
				if (git_buf_puts(&refname, head->name) < 0)
					goto on_error;
			}
		}

		/* If we didn't want to auto-follow the tag, check if the refspec matches */
		if (!autotag && git_refspec_src_matches(spec, head->name)) {
			if (spec->dst) {
				if (git_refspec_transform(&refname, spec, head->name) < 0)
					goto on_error;
			} else {
				/*
				 * no rhs mans store it in FETCH_HEAD, even if we don't
				 update anything else.
				 */
				if ((error = git_vector_insert(&update_heads, head)) < 0)
					goto on_error;

				continue;
			}
		}

		/* If we still don't have a refname, we don't want it */
		if (git_buf_len(&refname) == 0) {
			continue;
		}

		/* In autotag mode, only create tags for objects already in db */
		if (autotag && !git_odb_exists(odb, &head->oid))
			continue;

		if (!autotag && git_vector_insert(&update_heads, head) < 0)
			goto on_error;

		error = git_reference_name_to_id(&old, remote->repo, refname.ptr);
		if (error < 0 && error != GIT_ENOTFOUND)
			goto on_error;

		if (!(error || error == GIT_ENOTFOUND)
				&& !spec->force
				&& !git_graph_descendant_of(remote->repo, &head->oid, &old))
			continue;

		if (error == GIT_ENOTFOUND) {
			memset(&old, 0, GIT_OID_RAWSZ);

			if (autotag && git_vector_insert(&update_heads, head) < 0)
				goto on_error;
		}

		if (!git_oid__cmp(&old, &head->oid))
			continue;

		/* In autotag mode, don't overwrite any locally-existing tags */
		error = git_reference_create(&ref, remote->repo, refname.ptr, &head->oid, !autotag,
				log_message);

		if (error == GIT_EEXISTS)
			continue;

		if (error < 0)
			goto on_error;

		git_reference_free(ref);

		if (callbacks && callbacks->update_tips != NULL) {
			if (callbacks->update_tips(refname.ptr, &old, &head->oid, callbacks->payload) < 0)
				goto on_error;
		}
	}

	if (update_fetchhead &&
	    (error = git_remote_write_fetchhead(remote, spec, &update_heads)) < 0)
		goto on_error;

	git_vector_free(&update_heads);
	git_refspec__dispose(&tagspec);
	git_buf_dispose(&refname);
	return 0;

on_error:
	git_vector_free(&update_heads);
	git_refspec__dispose(&tagspec);
	git_buf_dispose(&refname);
	return -1;

}

/**
 * Iteration over the three vectors, with a pause whenever we find a match
 *
 * On each stop, we store the iteration stat in the inout i,j,k
 * parameters, and return the currently matching passive refspec as
 * well as the head which we matched.
 */
static int next_head(const git_remote *remote, git_vector *refs,
		     git_refspec **out_spec, git_remote_head **out_head,
		     size_t *out_i, size_t *out_j, size_t *out_k)
{
	const git_vector *active, *passive;
	git_remote_head *head;
	git_refspec *spec, *passive_spec;
	size_t i, j, k;
	int valid;

	active = &remote->active_refspecs;
	passive = &remote->passive_refspecs;

	i = *out_i;
	j = *out_j;
	k = *out_k;

	for (; i < refs->length; i++) {
		head = git_vector_get(refs, i);

		if (git_reference_name_is_valid(&valid, head->name) < 0)
			return -1;

		if (!valid)
			continue;

		for (; j < active->length; j++) {
			spec = git_vector_get(active, j);

			if (!git_refspec_src_matches(spec, head->name))
				continue;

			for (; k < passive->length; k++) {
				passive_spec = git_vector_get(passive, k);

				if (!git_refspec_src_matches(passive_spec, head->name))
				    continue;

				*out_spec = passive_spec;
				*out_head = head;
				*out_i = i;
				*out_j = j;
				*out_k = k + 1;
				return 0;

			}
			k = 0;
		}
		j = 0;
	}

	return GIT_ITEROVER;
}

static int opportunistic_updates(const git_remote *remote, const git_remote_callbacks *callbacks,
				 git_vector *refs, const char *msg)
{
	size_t i, j, k;
	git_refspec *spec;
	git_remote_head *head;
	git_reference *ref;
	git_buf refname = GIT_BUF_INIT;
	int error = 0;

	i = j = k = 0;

	while ((error = next_head(remote, refs, &spec, &head, &i, &j, &k)) == 0) {
		git_oid old = {{ 0 }};
		/*
		 * If we got here, there is a refspec which was used
		 * for fetching which matches the source of one of the
		 * passive refspecs, so we should update that
		 * remote-tracking branch, but not add it to
		 * FETCH_HEAD
		 */

		git_buf_clear(&refname);
		if ((error = git_refspec_transform(&refname, spec, head->name)) < 0)
			goto cleanup;

		error = git_reference_name_to_id(&old, remote->repo, refname.ptr);
		if (error < 0 && error != GIT_ENOTFOUND)
			goto cleanup;

		if (!git_oid_cmp(&old, &head->oid))
			continue;

		/* If we did find a current reference, make sure we haven't lost a race */
		if (error)
			error = git_reference_create(&ref, remote->repo, refname.ptr, &head->oid, true, msg);
		else
			error = git_reference_create_matching(&ref, remote->repo, refname.ptr, &head->oid, true, &old, msg);
		git_reference_free(ref);
		if (error < 0)
			goto cleanup;

		if (callbacks && callbacks->update_tips != NULL) {
			if (callbacks->update_tips(refname.ptr, &old, &head->oid, callbacks->payload) < 0)
				goto cleanup;
		}
	}

	if (error == GIT_ITEROVER)
		error = 0;

cleanup:
	git_buf_dispose(&refname);
	return error;
}

static int truncate_fetch_head(const char *gitdir)
{
	git_buf path = GIT_BUF_INIT;
	int error;

	if ((error = git_buf_joinpath(&path, gitdir, GIT_FETCH_HEAD_FILE)) < 0)
		return error;

	error = git_futils_truncate(path.ptr, GIT_REFS_FILE_MODE);
	git_buf_dispose(&path);

	return error;
}

int git_remote_update_tips(
		git_remote *remote,
		const git_remote_callbacks *callbacks,
		int update_fetchhead,
		git_remote_autotag_option_t download_tags,
		const char *reflog_message)
{
	git_refspec *spec, tagspec;
	git_vector refs = GIT_VECTOR_INIT;
	git_remote_autotag_option_t tagopt;
	int error;
	size_t i;

	/* push has its own logic hidden away in the push object */
	if (remote->push) {
		return git_push_update_tips(remote->push, callbacks);
	}

	if (git_refspec__parse(&tagspec, GIT_REFSPEC_TAGS, true) < 0)
		return -1;


	if ((error = ls_to_vector(&refs, remote)) < 0)
		goto out;

	if (download_tags == GIT_REMOTE_DOWNLOAD_TAGS_UNSPECIFIED)
		tagopt = remote->download_tags;
	else
		tagopt = download_tags;

	if ((error = truncate_fetch_head(git_repository_path(remote->repo))) < 0)
		goto out;

	if (tagopt == GIT_REMOTE_DOWNLOAD_TAGS_ALL) {
		if ((error = update_tips_for_spec(remote, callbacks, update_fetchhead, tagopt, &tagspec, &refs, reflog_message)) < 0)
			goto out;
	}

	git_vector_foreach(&remote->active_refspecs, i, spec) {
		if (spec->push)
			continue;

		if ((error = update_tips_for_spec(remote, callbacks, update_fetchhead, tagopt, spec, &refs, reflog_message)) < 0)
			goto out;
	}

	/* Only try to do opportunistic updates if the refpec lists differ. */
	if (remote->passed_refspecs)
		error = opportunistic_updates(remote, callbacks, &refs, reflog_message);

out:
	git_vector_free(&refs);
	git_refspec__dispose(&tagspec);
	return error;
}

int git_remote_connected(const git_remote *remote)
{
	GIT_ASSERT_ARG(remote);

	if (!remote->transport || !remote->transport->is_connected)
		return 0;

	/* Ask the transport if it's connected. */
	return remote->transport->is_connected(remote->transport);
}

int git_remote_stop(git_remote *remote)
{
	GIT_ASSERT_ARG(remote);

	if (remote->transport && remote->transport->cancel)
		remote->transport->cancel(remote->transport);

	return 0;
}

int git_remote_disconnect(git_remote *remote)
{
	GIT_ASSERT_ARG(remote);

	if (git_remote_connected(remote))
		remote->transport->close(remote->transport);

	return 0;
}

void git_remote_free(git_remote *remote)
{
	if (remote == NULL)
		return;

	if (remote->transport != NULL) {
		git_remote_disconnect(remote);

		remote->transport->free(remote->transport);
		remote->transport = NULL;
	}

	git_vector_free(&remote->refs);

	free_refspecs(&remote->refspecs);
	git_vector_free(&remote->refspecs);

	free_refspecs(&remote->active_refspecs);
	git_vector_free(&remote->active_refspecs);

	free_refspecs(&remote->passive_refspecs);
	git_vector_free(&remote->passive_refspecs);

	git_push_free(remote->push);
	git__free(remote->url);
	git__free(remote->pushurl);
	git__free(remote->name);
	git__free(remote);
}

static int remote_list_cb(const git_config_entry *entry, void *payload)
{
	git_vector *list = payload;
	const char *name = entry->name + strlen("remote.");
	size_t namelen = strlen(name);
	char *remote_name;

	/* we know name matches "remote.<stuff>.(push)?url" */

	if (!strcmp(&name[namelen - 4], ".url"))
		remote_name = git__strndup(name, namelen - 4); /* strip ".url" */
	else
		remote_name = git__strndup(name, namelen - 8); /* strip ".pushurl" */
	GIT_ERROR_CHECK_ALLOC(remote_name);

	return git_vector_insert(list, remote_name);
}

int git_remote_list(git_strarray *remotes_list, git_repository *repo)
{
	int error;
	git_config *cfg;
	git_vector list = GIT_VECTOR_INIT;

	if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
		return error;

	if ((error = git_vector_init(&list, 4, git__strcmp_cb)) < 0)
		return error;

	error = git_config_foreach_match(
		cfg, "^remote\\..*\\.(push)?url$", remote_list_cb, &list);

	if (error < 0) {
		git_vector_free_deep(&list);
		return error;
	}

	git_vector_uniq(&list, git__free);

	remotes_list->strings =
		(char **)git_vector_detach(&remotes_list->count, NULL, &list);

	return 0;
}

const git_indexer_progress *git_remote_stats(git_remote *remote)
{
	GIT_ASSERT_ARG_WITH_RETVAL(remote, NULL);
	return &remote->stats;
}

git_remote_autotag_option_t git_remote_autotag(const git_remote *remote)
{
	return remote->download_tags;
}

int git_remote_set_autotag(git_repository *repo, const char *remote, git_remote_autotag_option_t value)
{
	git_buf var = GIT_BUF_INIT;
	git_config *config;
	int error;

	GIT_ASSERT_ARG(repo && remote);

	if ((error = ensure_remote_name_is_valid(remote)) < 0)
		return error;

	if ((error = git_repository_config__weakptr(&config, repo)) < 0)
		return error;

	if ((error = git_buf_printf(&var, CONFIG_TAGOPT_FMT, remote)))
		return error;

	switch (value) {
	case GIT_REMOTE_DOWNLOAD_TAGS_NONE:
		error = git_config_set_string(config, var.ptr, "--no-tags");
		break;
	case GIT_REMOTE_DOWNLOAD_TAGS_ALL:
		error = git_config_set_string(config, var.ptr, "--tags");
		break;
	case GIT_REMOTE_DOWNLOAD_TAGS_AUTO:
		error = git_config_delete_entry(config, var.ptr);
		if (error == GIT_ENOTFOUND)
			error = 0;
		break;
	default:
		git_error_set(GIT_ERROR_INVALID, "invalid value for the tagopt setting");
		error = -1;
	}

	git_buf_dispose(&var);
	return error;
}

int git_remote_prune_refs(const git_remote *remote)
{
	return remote->prune_refs;
}

static int rename_remote_config_section(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	git_buf old_section_name = GIT_BUF_INIT,
		new_section_name = GIT_BUF_INIT;
	int error = -1;

	if (git_buf_printf(&old_section_name, "remote.%s", old_name) < 0)
		goto cleanup;

	if (new_name &&
		(git_buf_printf(&new_section_name, "remote.%s", new_name) < 0))
			goto cleanup;

	error = git_config_rename_section(
		repo,
		git_buf_cstr(&old_section_name),
		new_name ? git_buf_cstr(&new_section_name) : NULL);

cleanup:
	git_buf_dispose(&old_section_name);
	git_buf_dispose(&new_section_name);

	return error;
}

struct update_data {
	git_config *config;
	const char *old_remote_name;
	const char *new_remote_name;
};

static int update_config_entries_cb(
	const git_config_entry *entry,
	void *payload)
{
	struct update_data *data = (struct update_data *)payload;

	if (strcmp(entry->value, data->old_remote_name))
		return 0;

	return git_config_set_string(
		data->config, entry->name, data->new_remote_name);
}

static int update_branch_remote_config_entry(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	int error;
	struct update_data data = { NULL };

	if ((error = git_repository_config__weakptr(&data.config, repo)) < 0)
		return error;

	data.old_remote_name = old_name;
	data.new_remote_name = new_name;

	return git_config_foreach_match(
		data.config, "branch\\..+\\.remote", update_config_entries_cb, &data);
}

static int rename_one_remote_reference(
	git_reference *reference_in,
	const char *old_remote_name,
	const char *new_remote_name)
{
	int error;
	git_reference *ref = NULL, *dummy = NULL;
	git_buf namespace = GIT_BUF_INIT, old_namespace = GIT_BUF_INIT;
	git_buf new_name = GIT_BUF_INIT;
	git_buf log_message = GIT_BUF_INIT;
	size_t pfx_len;
	const char *target;

	if ((error = git_buf_printf(&namespace, GIT_REFS_REMOTES_DIR "%s/", new_remote_name)) < 0)
		return error;

	pfx_len = strlen(GIT_REFS_REMOTES_DIR) + strlen(old_remote_name) + 1;
	git_buf_puts(&new_name, namespace.ptr);
	if ((error = git_buf_puts(&new_name, git_reference_name(reference_in) + pfx_len)) < 0)
		goto cleanup;

	if ((error = git_buf_printf(&log_message,
					"renamed remote %s to %s",
					old_remote_name, new_remote_name)) < 0)
		goto cleanup;

	if ((error = git_reference_rename(&ref, reference_in, git_buf_cstr(&new_name), 1,
					  git_buf_cstr(&log_message))) < 0)
		goto cleanup;

	if (git_reference_type(ref) != GIT_REFERENCE_SYMBOLIC)
		goto cleanup;

	/* Handle refs like origin/HEAD -> origin/master */
	target = git_reference_symbolic_target(ref);
	if ((error = git_buf_printf(&old_namespace, GIT_REFS_REMOTES_DIR "%s/", old_remote_name)) < 0)
		goto cleanup;

	if (git__prefixcmp(target, old_namespace.ptr))
		goto cleanup;

	git_buf_clear(&new_name);
	git_buf_puts(&new_name, namespace.ptr);
	if ((error = git_buf_puts(&new_name, target + pfx_len)) < 0)
		goto cleanup;

	error = git_reference_symbolic_set_target(&dummy, ref, git_buf_cstr(&new_name),
						  git_buf_cstr(&log_message));

	git_reference_free(dummy);

cleanup:
	git_reference_free(reference_in);
	git_reference_free(ref);
	git_buf_dispose(&namespace);
	git_buf_dispose(&old_namespace);
	git_buf_dispose(&new_name);
	git_buf_dispose(&log_message);
	return error;
}

static int rename_remote_references(
	git_repository *repo,
	const char *old_name,
	const char *new_name)
{
	int error;
	git_buf buf = GIT_BUF_INIT;
	git_reference *ref;
	git_reference_iterator *iter;

	if ((error = git_buf_printf(&buf, GIT_REFS_REMOTES_DIR "%s/*", old_name)) < 0)
		return error;

	error = git_reference_iterator_glob_new(&iter, repo, git_buf_cstr(&buf));
	git_buf_dispose(&buf);

	if (error < 0)
		return error;

	while ((error = git_reference_next(&ref, iter)) == 0) {
		if ((error = rename_one_remote_reference(ref, old_name, new_name)) < 0)
			break;
	}

	git_reference_iterator_free(iter);

	return (error == GIT_ITEROVER) ? 0 : error;
}

static int rename_fetch_refspecs(git_vector *problems, git_remote *remote, const char *new_name)
{
	git_config *config;
	git_buf base = GIT_BUF_INIT, var = GIT_BUF_INIT, val = GIT_BUF_INIT;
	const git_refspec *spec;
	size_t i;
	int error = 0;

	if ((error = git_repository_config__weakptr(&config, remote->repo)) < 0)
		return error;

	if ((error = git_vector_init(problems, 1, NULL)) < 0)
		return error;

	if ((error = default_fetchspec_for_name(&base, remote->name)) < 0)
		return error;

	git_vector_foreach(&remote->refspecs, i, spec) {
		if (spec->push)
			continue;

		/* Does the dst part of the refspec follow the expected format? */
		if (strcmp(git_buf_cstr(&base), spec->string)) {
			char *dup;

			dup = git__strdup(spec->string);
			GIT_ERROR_CHECK_ALLOC(dup);

			if ((error = git_vector_insert(problems, dup)) < 0)
				break;

			continue;
		}

		/* If we do want to move it to the new section */

		git_buf_clear(&val);
		git_buf_clear(&var);

		if (default_fetchspec_for_name(&val, new_name) < 0 ||
			git_buf_printf(&var, "remote.%s.fetch", new_name) < 0)
		{
			error = -1;
			break;
		}

		if ((error = git_config_set_string(
				config, git_buf_cstr(&var), git_buf_cstr(&val))) < 0)
			break;
	}

	git_buf_dispose(&base);
	git_buf_dispose(&var);
	git_buf_dispose(&val);

	if (error < 0) {
		char *str;
		git_vector_foreach(problems, i, str)
			git__free(str);

		git_vector_free(problems);
	}

	return error;
}

int git_remote_rename(git_strarray *out, git_repository *repo, const char *name, const char *new_name)
{
	int error;
	git_vector problem_refspecs = GIT_VECTOR_INIT;
	git_remote *remote = NULL;

	GIT_ASSERT_ARG(out && repo && name && new_name);

	if ((error = git_remote_lookup(&remote, repo, name)) < 0)
		return error;

	if ((error = ensure_remote_name_is_valid(new_name)) < 0)
		goto cleanup;

	if ((error = ensure_remote_doesnot_exist(repo, new_name)) < 0)
		goto cleanup;

	if ((error = rename_remote_config_section(repo, name, new_name)) < 0)
		goto cleanup;

	if ((error = update_branch_remote_config_entry(repo, name, new_name)) < 0)
		goto cleanup;

	if ((error = rename_remote_references(repo, name, new_name)) < 0)
		goto cleanup;

	if ((error = rename_fetch_refspecs(&problem_refspecs, remote, new_name)) < 0)
		goto cleanup;

	out->count = problem_refspecs.length;
	out->strings = (char **) problem_refspecs.contents;

cleanup:
	if (error < 0)
		git_vector_free(&problem_refspecs);

	git_remote_free(remote);
	return error;
}

int git_remote_name_is_valid(int *valid, const char *remote_name)
{
	git_buf buf = GIT_BUF_INIT;
	git_refspec refspec = {0};
	int error;

	GIT_ASSERT(valid);

	*valid = 0;

	if (!remote_name || *remote_name == '\0')
		return 0;

	if ((error = git_buf_printf(&buf, "refs/heads/test:refs/remotes/%s/test", remote_name)) < 0)
		goto done;

	error = git_refspec__parse(&refspec, git_buf_cstr(&buf), true);

	if (!error)
		*valid = 1;
	else if (error == GIT_EINVALIDSPEC)
		error = 0;

done:
	git_buf_dispose(&buf);
	git_refspec__dispose(&refspec);

	return error;
}

git_refspec *git_remote__matching_refspec(git_remote *remote, const char *refname)
{
	git_refspec *spec;
	size_t i;

	git_vector_foreach(&remote->active_refspecs, i, spec) {
		if (spec->push)
			continue;

		if (git_refspec_src_matches(spec, refname))
			return spec;
	}

	return NULL;
}

git_refspec *git_remote__matching_dst_refspec(git_remote *remote, const char *refname)
{
	git_refspec *spec;
	size_t i;

	git_vector_foreach(&remote->active_refspecs, i, spec) {
		if (spec->push)
			continue;

		if (git_refspec_dst_matches(spec, refname))
			return spec;
	}

	return NULL;
}

int git_remote_add_fetch(git_repository *repo, const char *remote, const char *refspec)
{
	return write_add_refspec(repo, remote, refspec, true);
}

int git_remote_add_push(git_repository *repo, const char *remote, const char *refspec)
{
	return write_add_refspec(repo, remote, refspec, false);
}

static int copy_refspecs(git_strarray *array, const git_remote *remote, unsigned int push)
{
	size_t i;
	git_vector refspecs;
	git_refspec *spec;
	char *dup;

	if (git_vector_init(&refspecs, remote->refspecs.length, NULL) < 0)
		return -1;

	git_vector_foreach(&remote->refspecs, i, spec) {
		if (spec->push != push)
			continue;

		if ((dup = git__strdup(spec->string)) == NULL)
			goto on_error;

		if (git_vector_insert(&refspecs, dup) < 0) {
			git__free(dup);
			goto on_error;
		}
	}

	array->strings = (char **)refspecs.contents;
	array->count = refspecs.length;

	return 0;

on_error:
	git_vector_free_deep(&refspecs);

	return -1;
}

int git_remote_get_fetch_refspecs(git_strarray *array, const git_remote *remote)
{
	return copy_refspecs(array, remote, false);
}

int git_remote_get_push_refspecs(git_strarray *array, const git_remote *remote)
{
	return copy_refspecs(array, remote, true);
}

size_t git_remote_refspec_count(const git_remote *remote)
{
	return remote->refspecs.length;
}

const git_refspec *git_remote_get_refspec(const git_remote *remote, size_t n)
{
	return git_vector_get(&remote->refspecs, n);
}

int git_remote_init_callbacks(git_remote_callbacks *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_remote_callbacks, GIT_REMOTE_CALLBACKS_INIT);
	return 0;
}

/* asserts a branch.<foo>.remote format */
static const char *name_offset(size_t *len_out, const char *name)
{
	size_t prefix_len;
	const char *dot;

	prefix_len = strlen("remote.");
	dot = strchr(name + prefix_len, '.');

	GIT_ASSERT_ARG_WITH_RETVAL(dot, NULL);

	*len_out = dot - name - prefix_len;
	return name + prefix_len;
}

static int remove_branch_config_related_entries(
	git_repository *repo,
	const char *remote_name)
{
	int error;
	git_config *config;
	git_config_entry *entry;
	git_config_iterator *iter;
	git_buf buf = GIT_BUF_INIT;

	if ((error = git_repository_config__weakptr(&config, repo)) < 0)
		return error;

	if ((error = git_config_iterator_glob_new(&iter, config, "branch\\..+\\.remote")) < 0)
		return error;

	/* find any branches with us as upstream and remove that config */
	while ((error = git_config_next(&entry, iter)) == 0) {
		const char *branch;
		size_t branch_len;

		if (strcmp(remote_name, entry->value))
			continue;

		if ((branch = name_offset(&branch_len, entry->name)) == NULL) {
			error = -1;
			break;
		}

		git_buf_clear(&buf);
		if ((error = git_buf_printf(&buf, "branch.%.*s.merge", (int)branch_len, branch)) < 0)
			break;

		if ((error = git_config_delete_entry(config, git_buf_cstr(&buf))) < 0) {
			if (error != GIT_ENOTFOUND)
				break;
			git_error_clear();
		}

		git_buf_clear(&buf);
		if ((error = git_buf_printf(&buf, "branch.%.*s.remote", (int)branch_len, branch)) < 0)
			break;

		if ((error = git_config_delete_entry(config, git_buf_cstr(&buf))) < 0) {
			if (error != GIT_ENOTFOUND)
				break;
			git_error_clear();
		}
	}

	if (error == GIT_ITEROVER)
		error = 0;

	git_buf_dispose(&buf);
	git_config_iterator_free(iter);
	return error;
}

static int remove_refs(git_repository *repo, const git_refspec *spec)
{
	git_reference_iterator *iter = NULL;
	git_vector refs;
	const char *name;
	char *dup;
	int error;
	size_t i;

	if ((error = git_vector_init(&refs, 8, NULL)) < 0)
		return error;

	if ((error = git_reference_iterator_new(&iter, repo)) < 0)
		goto cleanup;

	while ((error = git_reference_next_name(&name, iter)) == 0) {
		if (!git_refspec_dst_matches(spec, name))
			continue;

		dup = git__strdup(name);
		if (!dup) {
			error = -1;
			goto cleanup;
		}

		if ((error = git_vector_insert(&refs, dup)) < 0)
			goto cleanup;
	}
	if (error == GIT_ITEROVER)
		error = 0;
	if (error < 0)
		goto cleanup;

	git_vector_foreach(&refs, i, name) {
		if ((error = git_reference_remove(repo, name)) < 0)
			break;
	}

cleanup:
	git_reference_iterator_free(iter);
	git_vector_foreach(&refs, i, dup) {
		git__free(dup);
	}
	git_vector_free(&refs);
	return error;
}

static int remove_remote_tracking(git_repository *repo, const char *remote_name)
{
	git_remote *remote;
	int error;
	size_t i, count;

	/* we want to use what's on the config, regardless of changes to the instance in memory */
	if ((error = git_remote_lookup(&remote, repo, remote_name)) < 0)
		return error;

	count = git_remote_refspec_count(remote);
	for (i = 0; i < count; i++) {
		const git_refspec *refspec = git_remote_get_refspec(remote, i);

		/* shouldn't ever actually happen */
		if (refspec == NULL)
			continue;

		if ((error = remove_refs(repo, refspec)) < 0)
			break;
	}

	git_remote_free(remote);
	return error;
}

int git_remote_delete(git_repository *repo, const char *name)
{
	int error;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);

	if ((error = remove_branch_config_related_entries(repo, name)) < 0 ||
	    (error = remove_remote_tracking(repo, name)) < 0 ||
	    (error = rename_remote_config_section(repo, name, NULL)) < 0)
		return error;

	return 0;
}

int git_remote_default_branch(git_buf *out, git_remote *remote)
{
	const git_remote_head **heads;
	const git_remote_head *guess = NULL;
	const git_oid *head_id;
	size_t heads_len, i;
	git_buf local_default = GIT_BUF_INIT;
	int error;

	GIT_ASSERT_ARG(out);

	if ((error = git_remote_ls(&heads, &heads_len, remote)) < 0)
		goto done;

	if (heads_len == 0 || strcmp(heads[0]->name, GIT_HEAD_FILE)) {
		error = GIT_ENOTFOUND;
		goto done;
	}

	if ((error = git_buf_sanitize(out)) < 0)
		return error;

	/* the first one must be HEAD so if that has the symref info, we're done */
	if (heads[0]->symref_target) {
		error = git_buf_puts(out, heads[0]->symref_target);
		goto done;
	}

	/*
	 * If there's no symref information, we have to look over them
	 * and guess. We return the first match unless the default
	 * branch is a candidate. Then we return the default branch.
	 */

	if ((error = git_repository_initialbranch(&local_default, remote->repo)) < 0)
		goto done;

	head_id = &heads[0]->oid;

	for (i = 1; i < heads_len; i++) {
		if (git_oid_cmp(head_id, &heads[i]->oid))
			continue;

		if (git__prefixcmp(heads[i]->name, GIT_REFS_HEADS_DIR))
			continue;

		if (!guess) {
			guess = heads[i];
			continue;
		}

		if (!git__strcmp(local_default.ptr, heads[i]->name)) {
			guess = heads[i];
			break;
		}
	}

	if (!guess) {
		error = GIT_ENOTFOUND;
		goto done;
	}

	error = git_buf_puts(out, guess->name);

done:
	git_buf_dispose(&local_default);
	return error;
}

int git_remote_upload(git_remote *remote, const git_strarray *refspecs, const git_push_options *opts)
{
	size_t i;
	int error;
	git_push *push;
	git_refspec *spec;
	const git_remote_callbacks *cbs = NULL;
	git_remote_connection_opts conn = GIT_REMOTE_CONNECTION_OPTIONS_INIT;

	GIT_ASSERT_ARG(remote);

	if (!remote->repo) {
		git_error_set(GIT_ERROR_INVALID, "cannot download detached remote");
		return -1;
	}

	if (opts) {
		cbs = &opts->callbacks;
		conn.custom_headers = &opts->custom_headers;
		conn.proxy = &opts->proxy_opts;
	}

	if (!git_remote_connected(remote) &&
	    (error = git_remote__connect(remote, GIT_DIRECTION_PUSH, cbs, &conn)) < 0)
		goto cleanup;

	free_refspecs(&remote->active_refspecs);
	if ((error = dwim_refspecs(&remote->active_refspecs, &remote->refspecs, &remote->refs)) < 0)
		goto cleanup;

	if (remote->push) {
		git_push_free(remote->push);
		remote->push = NULL;
	}

	if ((error = git_push_new(&remote->push, remote)) < 0)
		return error;

	push = remote->push;

	if (opts && (error = git_push_set_options(push, opts)) < 0)
		goto cleanup;

	if (refspecs && refspecs->count > 0) {
		for (i = 0; i < refspecs->count; i++) {
			if ((error = git_push_add_refspec(push, refspecs->strings[i])) < 0)
				goto cleanup;
		}
	} else {
		git_vector_foreach(&remote->refspecs, i, spec) {
			if (!spec->push)
				continue;
			if ((error = git_push_add_refspec(push, spec->string)) < 0)
				goto cleanup;
		}
	}

	if ((error = git_push_finish(push, cbs)) < 0)
		goto cleanup;

	if (cbs && cbs->push_update_reference &&
	    (error = git_push_status_foreach(push, cbs->push_update_reference, cbs->payload)) < 0)
		goto cleanup;

cleanup:
	return error;
}

int git_remote_push(git_remote *remote, const git_strarray *refspecs, const git_push_options *opts)
{
	int error;
	const git_remote_callbacks *cbs = NULL;
	const git_strarray *custom_headers = NULL;
	const git_proxy_options *proxy = NULL;

	GIT_ASSERT_ARG(remote);

	if (!remote->repo) {
		git_error_set(GIT_ERROR_INVALID, "cannot download detached remote");
		return -1;
	}

	if (opts) {
		GIT_ERROR_CHECK_VERSION(&opts->callbacks, GIT_REMOTE_CALLBACKS_VERSION, "git_remote_callbacks");
		cbs = &opts->callbacks;
		custom_headers = &opts->custom_headers;
		GIT_ERROR_CHECK_VERSION(&opts->proxy_opts, GIT_PROXY_OPTIONS_VERSION, "git_proxy_options");
		proxy = &opts->proxy_opts;
	}

	if ((error = git_remote_connect(remote, GIT_DIRECTION_PUSH, cbs, proxy, custom_headers)) < 0)
		return error;

	if ((error = git_remote_upload(remote, refspecs, opts)) < 0)
		return error;

	error = git_remote_update_tips(remote, cbs, 0, 0, NULL);

	git_remote_disconnect(remote);
	return error;
}

#define PREFIX "url"
#define SUFFIX_FETCH "insteadof"
#define SUFFIX_PUSH "pushinsteadof"

char *apply_insteadof(git_config *config, const char *url, int direction)
{
	size_t match_length, prefix_length, suffix_length;
	char *replacement = NULL;
	const char *regexp;

	git_buf result = GIT_BUF_INIT;
	git_config_entry *entry;
	git_config_iterator *iter;

	GIT_ASSERT_ARG_WITH_RETVAL(config, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(url, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(direction == GIT_DIRECTION_FETCH || direction == GIT_DIRECTION_PUSH, NULL);

	/* Add 1 to prefix/suffix length due to the additional escaped dot */
	prefix_length = strlen(PREFIX) + 1;
	if (direction == GIT_DIRECTION_FETCH) {
		regexp = PREFIX "\\..*\\." SUFFIX_FETCH;
		suffix_length = strlen(SUFFIX_FETCH) + 1;
	} else {
		regexp = PREFIX "\\..*\\." SUFFIX_PUSH;
		suffix_length = strlen(SUFFIX_PUSH) + 1;
	}

	if (git_config_iterator_glob_new(&iter, config, regexp) < 0)
		return NULL;

	match_length = 0;
	while (git_config_next(&entry, iter) == 0) {
		size_t n, replacement_length;

		/* Check if entry value is a prefix of URL */
		if (git__prefixcmp(url, entry->value))
			continue;
		/* Check if entry value is longer than previous
		 * prefixes */
		if ((n = strlen(entry->value)) <= match_length)
			continue;

		git__free(replacement);
		match_length = n;

		/* Cut off prefix and suffix of the value */
		replacement_length =
		    strlen(entry->name) - (prefix_length + suffix_length);
		replacement = git__strndup(entry->name + prefix_length,
				replacement_length);
	}

	git_config_iterator_free(iter);

	if (match_length == 0)
		return git__strdup(url);

	git_buf_printf(&result, "%s%s", replacement, url + match_length);

	git__free(replacement);

	return result.ptr;
}

/* Deprecated functions */

#ifndef GIT_DEPRECATE_HARD

int git_remote_is_valid_name(const char *remote_name)
{
	int valid = 0;

	git_remote_name_is_valid(&valid, remote_name);
	return valid;
}

#endif
