#ifndef BUNDLE_URI_H
#define BUNDLE_URI_H

#include "hashmap.h"
#include "strbuf.h"

struct repository;
struct string_list;

/**
 * The remote_bundle_info struct contains information for a single bundle
 * URI. This may be initialized simply by a given URI or might have
 * additional metadata associated with it if the bundle was advertised by
 * a bundle list.
 */
struct remote_bundle_info {
	struct hashmap_entry ent;

	/**
	 * The 'id' is a name given to the bundle for reference
	 * by other bundle infos.
	 */
	char *id;

	/**
	 * The 'uri' is the location of the remote bundle so
	 * it can be downloaded on-demand. This will be NULL
	 * if there was no table of contents.
	 */
	char *uri;
};

#define REMOTE_BUNDLE_INFO_INIT { 0 }

enum bundle_list_mode {
	BUNDLE_MODE_NONE = 0,
	BUNDLE_MODE_ALL,
	BUNDLE_MODE_ANY
};

/**
 * A bundle_list contains an unordered set of remote_bundle_info structs,
 * as well as information about the bundle listing, such as version and
 * mode.
 */
struct bundle_list {
	int version;
	enum bundle_list_mode mode;
	struct hashmap bundles;
};

void init_bundle_list(struct bundle_list *list);
void clear_bundle_list(struct bundle_list *list);

typedef int (*bundle_iterator)(struct remote_bundle_info *bundle,
			       void *data);

int for_all_bundles_in_list(struct bundle_list *list,
			    bundle_iterator iter,
			    void *data);

/**
 * Fetch data from the given 'uri' and unbundle the bundle data found
 * based on that information.
 *
 * Returns non-zero if no bundle information is found at the given 'uri'.
 */
int fetch_bundle_uri(struct repository *r, const char *uri);

#endif
