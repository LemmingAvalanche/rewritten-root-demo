#ifndef CREDENTIAL_H
#define CREDENTIAL_H

#include "string-list.h"

/**
 * The credentials API provides an abstracted way of gathering username and
 * password credentials from the user.
 *
 * Typical setup
 * -------------
 *
 * ------------
 * +-----------------------+
 * | Git code (C)          |--- to server requiring --->
 * |                       |        authentication
 * |.......................|
 * | C credential API      |--- prompt ---> User
 * +-----------------------+
 * 	^      |
 * 	| pipe |
 * 	|      v
 * +-----------------------+
 * | Git credential helper |
 * +-----------------------+
 * ------------
 *
 * The Git code (typically a remote-helper) will call the C API to obtain
 * credential data like a login/password pair (credential_fill). The
 * API will itself call a remote helper (e.g. "git credential-cache" or
 * "git credential-store") that may retrieve credential data from a
 * store. If the credential helper cannot find the information, the C API
 * will prompt the user. Then, the caller of the API takes care of
 * contacting the server, and does the actual authentication.
 *
 * C API
 * -----
 *
 * The credential C API is meant to be called by Git code which needs to
 * acquire or store a credential. It is centered around an object
 * representing a single credential and provides three basic operations:
 * fill (acquire credentials by calling helpers and/or prompting the user),
 * approve (mark a credential as successfully used so that it can be stored
 * for later use), and reject (mark a credential as unsuccessful so that it
 * can be erased from any persistent storage).
 *
 * Example
 * ~~~~~~~
 *
 * The example below shows how the functions of the credential API could be
 * used to login to a fictitious "foo" service on a remote host:
 *
 * -----------------------------------------------------------------------
 * int foo_login(struct foo_connection *f)
 * {
 * 	int status;
 * 	// Create a credential with some context; we don't yet know the
 * 	// username or password.
 *
 * struct credential c = CREDENTIAL_INIT;
 * c.protocol = xstrdup("foo");
 * c.host = xstrdup(f->hostname);
 *
 * // Fill in the username and password fields by contacting
 * // helpers and/or asking the user. The function will die if it
 * // fails.
 * credential_fill(&c);
 *
 * // Otherwise, we have a username and password. Try to use it.
 *
 * status = send_foo_login(f, c.username, c.password);
 * switch (status) {
 * case FOO_OK:
 * // It worked. Store the credential for later use.
 * credential_accept(&c);
 * break;
 * case FOO_BAD_LOGIN:
 * // Erase the credential from storage so we don't try it again.
 * credential_reject(&c);
 * break;
 * default:
 * // Some other error occurred. We don't know if the
 * // credential is good or bad, so report nothing to the
 * // credential subsystem.
 * }
 *
 * // Free any associated resources.
 * credential_clear(&c);
 *
 * return status;
 * }
 * -----------------------------------------------------------------------
 *
 * Credential Helpers
 * ------------------
 *
 * Credential helpers are programs executed by Git to fetch or save
 * credentials from and to long-term storage (where "long-term" is simply
 * longer than a single Git process; e.g., credentials may be stored
 * in-memory for a few minutes, or indefinitely on disk).
 *
 * Each helper is specified by a single string in the configuration
 * variable `credential.helper` (and others, see Documentation/git-config.txt).
 * The string is transformed by Git into a command to be executed using
 * these rules:
 *
 *   1. If the helper string begins with "!", it is considered a shell
 *      snippet, and everything after the "!" becomes the command.
 *
 *   2. Otherwise, if the helper string begins with an absolute path, the
 *      verbatim helper string becomes the command.
 *
 *   3. Otherwise, the string "git credential-" is prepended to the helper
 *      string, and the result becomes the command.
 *
 * The resulting command then has an "operation" argument appended to it
 * (see below for details), and the result is executed by the shell.
 *
 * Here are some example specifications:
 *
 * ----------------------------------------------------
 * # run "git credential-foo"
 * foo
 *
 * # same as above, but pass an argument to the helper
 * foo --bar=baz
 *
 * # the arguments are parsed by the shell, so use shell
 * # quoting if necessary
 * foo --bar="whitespace arg"
 *
 * # you can also use an absolute path, which will not use the git wrapper
 * /path/to/my/helper --with-arguments
 *
 * # or you can specify your own shell snippet
 * !f() { echo "password=`cat $HOME/.secret`"; }; f
 * ----------------------------------------------------
 *
 * Generally speaking, rule (3) above is the simplest for users to specify.
 * Authors of credential helpers should make an effort to assist their
 * users by naming their program "git-credential-$NAME", and putting it in
 * the $PATH or $GIT_EXEC_PATH during installation, which will allow a user
 * to enable it with `git config credential.helper $NAME`.
 *
 * When a helper is executed, it will have one "operation" argument
 * appended to its command line, which is one of:
 *
 * `get`::
 *
 * 	Return a matching credential, if any exists.
 *
 * `store`::
 *
 * 	Store the credential, if applicable to the helper.
 *
 * `erase`::
 *
 * 	Remove a matching credential, if any, from the helper's storage.
 *
 * The details of the credential will be provided on the helper's stdin
 * stream. The exact format is the same as the input/output format of the
 * `git credential` plumbing command (see the section `INPUT/OUTPUT
 * FORMAT` in Documentation/git-credential.txt for a detailed specification).
 *
 * For a `get` operation, the helper should produce a list of attributes
 * on stdout in the same format. A helper is free to produce a subset, or
 * even no values at all if it has nothing useful to provide. Any provided
 * attributes will overwrite those already known about by Git.  If a helper
 * outputs a `quit` attribute with a value of `true` or `1`, no further
 * helpers will be consulted, nor will the user be prompted (if no
 * credential has been provided, the operation will then fail).
 *
 * For a `store` or `erase` operation, the helper's output is ignored.
 * If it fails to perform the requested operation, it may complain to
 * stderr to inform the user. If it does not support the requested
 * operation (e.g., a read-only store), it should silently ignore the
 * request.
 *
 * If a helper receives any other operation, it should silently ignore the
 * request. This leaves room for future operations to be added (older
 * helpers will just ignore the new requests).
 *
 */


/**
 * This struct represents a single username/password combination
 * along with any associated context. All string fields should be
 * heap-allocated (or NULL if they are not known or not applicable).
 * The meaning of the individual context fields is the same as
 * their counterparts in the helper protocol.
 *
 * This struct should always be initialized with `CREDENTIAL_INIT` or
 * `credential_init`.
 */
struct credential {

	/**
	 * A `string_list` of helpers. Each string specifies an external
	 * helper which will be run, in order, to either acquire or store
	 * credentials. This list is filled-in by the API functions
	 * according to the corresponding configuration variables before
	 * consulting helpers, so there usually is no need for a caller to
	 * modify the helpers field at all.
	 */
	struct string_list helpers;

	unsigned approved:1,
		 configured:1,
		 quit:1,
		 use_http_path:1;

	char *username;
	char *password;
	char *protocol;
	char *host;
	char *path;
};

#define CREDENTIAL_INIT { STRING_LIST_INIT_DUP }

/* Initialize a credential structure, setting all fields to empty. */
void credential_init(struct credential *);

/**
 * Free any resources associated with the credential structure, returning
 * it to a pristine initialized state.
 */
void credential_clear(struct credential *);

/**
 * Instruct the credential subsystem to fill the username and
 * password fields of the passed credential struct by first
 * consulting helpers, then asking the user. After this function
 * returns, the username and password fields of the credential are
 * guaranteed to be non-NULL. If an error occurs, the function will
 * die().
 */
void credential_fill(struct credential *);

/**
 * Inform the credential subsystem that the provided credentials
 * were successfully used for authentication.  This will cause the
 * credential subsystem to notify any helpers of the approval, so
 * that they may store the result to be used again.  Any errors
 * from helpers are ignored.
 */
void credential_approve(struct credential *);

/**
 * Inform the credential subsystem that the provided credentials
 * have been rejected. This will cause the credential subsystem to
 * notify any helpers of the rejection (which allows them, for
 * example, to purge the invalid credentials from storage). It
 * will also free() the username and password fields of the
 * credential and set them to NULL (readying the credential for
 * another call to `credential_fill`). Any errors from helpers are
 * ignored.
 */
void credential_reject(struct credential *);

int credential_read(struct credential *, FILE *);
void credential_write(const struct credential *, FILE *);

/* Parse a URL into broken-down credential fields. */
void credential_from_url(struct credential *, const char *url);

int credential_match(const struct credential *have,
		     const struct credential *want);

#endif /* CREDENTIAL_H */
