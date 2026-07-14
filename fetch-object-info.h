#ifndef FETCH_OBJECT_INFO_H
#define FETCH_OBJECT_INFO_H

#include "pkt-line.h"
#include "protocol.h"
#include "odb.h"

struct object_info_args {
	struct string_list *object_info_options;
	const struct string_list *server_options;
	struct oid_array *oids;
};

enum fetch_object_info_result {
	FETCH_OBJECT_INFO_OK = 0,
	FETCH_OBJECT_INFO_ERROR = -1,
	FETCH_OBJECT_INFO_NOT_ENABLED = -2,
	FETCH_OBJECT_INFO_UNSUPPORTED_PROTOCOL = -3,
};

/*
 * Sends git-cat-file object-info command into the request buf and read the
 * results from packets.
 */
enum fetch_object_info_result fetch_object_info(enum protocol_version version, struct object_info_args *args,
		      struct packet_reader *reader, struct object_info *object_info_data,
		      int stateless_rpc, int fd_out);

#endif /* FETCH_OBJECT_INFO_H */
