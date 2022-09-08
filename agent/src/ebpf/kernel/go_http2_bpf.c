static __inline void *get_the_first_parameter(struct pt_regs *ctx,
					      struct ebpf_proc_info *info)
{
	void *ptr;
	if (info->version >= GO_VERSION(1, 17, 0)) {
		ptr = (void *)ctx->rax;
	} else {
		bpf_probe_read(&ptr, sizeof(ptr), (void *)(ctx->rsp + 8));
	}
	return ptr;
}

static __inline bool is_grpc_syscallConn_interface(void *ptr,
						   struct ebpf_proc_info *info)
{
	struct go_interface i;
	bpf_probe_read(&i, sizeof(i), ptr);
	return info ? i.type == info->credentials_syscallConn_itab : false;
}

static __inline int get_fd_from_http2serverConn_ctx(struct pt_regs *ctx,
						    struct ebpf_proc_info *info)
{
	update_http2_tls(false);
	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_CONN_HTTP2_SERVER_CONN];
	return get_fd_from_tcp_or_tls_conn_interface(ptr, info);
}

static __inline int get_fd_from_http2ClientConn(void *ptr,
						struct ebpf_proc_info *info)
{
	update_http2_tls(false);
	ptr += info->offsets[OFFSET_IDX_TCONN_HTTP2_CLIENT_CONN];
	return get_fd_from_tcp_or_tls_conn_interface(ptr, info);
}

static __inline int get_fd_from_http2ClientConn_ctx(struct pt_regs *ctx,
						    struct ebpf_proc_info *info)
{
	void *ptr = get_the_first_parameter(ctx, info);
	return get_fd_from_http2ClientConn(ptr, info);
}

static __inline int
get_fd_from_grpc_http2Client_ctx(struct pt_regs *ctx,
				 struct ebpf_proc_info *info)
{
	update_http2_tls(false);
	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_CONN_GRPC_HTTP2_CLIENT];
	if (is_grpc_syscallConn_interface(ptr, info)) {
		update_http2_tls(true);
		struct go_interface i;
		bpf_probe_read(&i, sizeof(i), ptr);
		bpf_probe_read(&i, sizeof(i), i.ptr);
		ptr = i.ptr;
	}
	return get_fd_from_tcp_or_tls_conn_interface(ptr, info);
}

static __inline int
get_fd_from_grpc_http2Server_ctx(struct pt_regs *ctx,
				 struct ebpf_proc_info *info)
{
	update_http2_tls(false);
	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_CONN_GRPC_HTTP2_SERVER];
	if (is_grpc_syscallConn_interface(ptr, info)) {
		update_http2_tls(true);
		struct go_interface i;
		bpf_probe_read(&i, sizeof(i), ptr);
		bpf_probe_read(&i, sizeof(i), i.ptr);
		ptr = i.ptr;
	}
	return get_fd_from_tcp_or_tls_conn_interface(ptr, info);
}

static __inline int get_side_from_grpc_loopyWriter(struct pt_regs *ctx,
						   struct ebpf_proc_info *info)
{
	void *ptr = get_the_first_parameter(ctx, info);

	ptr += info->offsets[OFFSET_IDX_SIDE_GRPC_TRANSPORT_LOOPY_WRITER];
	int side = 0;
	bpf_probe_read(&side, sizeof(side), ptr);
	return side;
}

static __inline int get_fd_from_grpc_loopyWriter(struct pt_regs *ctx,
						 struct ebpf_proc_info *info)
{
	update_http2_tls(false);
	void *ptr = get_the_first_parameter(ctx, info);

	ptr += info->offsets[OFFSET_IDX_FRAMER_GRPC_TRANSPORT_LOOPY_WRITER];
	bpf_probe_read(&ptr, sizeof(ptr), ptr);
	ptr += info->offsets[OFFSET_IDX_WRITER_GRPC_TRANSPORT_FRAMER];
	bpf_probe_read(&ptr, sizeof(ptr), ptr);
	ptr += info->offsets[OFFSET_IDX_CONN_GRPC_TRANSPORT_BUFWRITER];

	if (is_grpc_syscallConn_interface(ptr, info)) {
		update_http2_tls(true);
		struct go_interface i;
		bpf_probe_read(&i, sizeof(i), ptr);
		bpf_probe_read(&i, sizeof(i), i.ptr);
		ptr = i.ptr;
	}
	return get_fd_from_tcp_or_tls_conn_interface(ptr, info);
}

struct go_http2_header_field {
	struct go_string name;
	struct go_string value;
	bool sensitive;
};

static __inline void *get_http2ClientConn_from_http2clientConnReadLoop_ctx(
	struct pt_regs *ctx, struct ebpf_proc_info *info)
{
	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_CC_HTTP2_CLIENT_CONN_READ_LOOP];
	bpf_probe_read(&ptr, sizeof(ptr), ptr);
	return ptr;
}

static __inline int
get_fd_from_http2clientConnReadLoop_ctx(struct pt_regs *ctx,
					struct ebpf_proc_info *info)
{
	void *ptr =
		get_http2ClientConn_from_http2clientConnReadLoop_ctx(ctx, info);
	return get_fd_from_http2ClientConn(ptr, info);
}

static __inline __u32 get_previous_read_tcp_seq(int fd, __u32 seq_end)
{
	struct http2_tcp_seq_key key = {
		.tgid = bpf_get_current_pid_tgid() >> 32,
		.fd = fd,
		.tcp_seq_end = seq_end,
	};
	__u32 *seq_begin = bpf_map_lookup_elem(&http2_tcp_seq_map, &key);
	if (seq_begin) {
		return *seq_begin;
	}
	return 0;
}

struct http2_header_data {
	// The read operation must be INGRESS, otherwise EGRESS
	bool read : 1;

	// Client-side write and server-side read are marked as requests,
	// otherwise marked as responses
	enum message_type message_type;

	// Fields that need to be serialized
	int fd;
	struct go_string name;
	struct go_string value;
	__u32 stream;
	struct pt_regs *ctx;
};

// Take data from extra and send it, and a __http2_buffer used as a big stack
static __inline void report_http2_header(struct pt_regs *ctx)
{
	if (!ctx) {
		return;
	}
	struct __http2_stack *stack = get_http2_stack();

	if (!stack) {
		return;
	}

	stack->events_num = 1;
	stack->len = offsetof(typeof(struct __socket_data), data) +
		     stack->send_buffer.syscall_len;

	__u32 send_size = (stack->len + 8) & 1023;
	bpf_perf_event_output(ctx, &NAME(socket_data), BPF_F_CURRENT_CPU,
			      &(stack->__raw), 1 + send_size);
}

// Fill all fields except data in buffer->send_buffer
static __inline void http2_fill_common_socket(struct http2_header_data *data,
					      struct __socket_data *send_buffer)
{
	// source, coroutine_id, timestamp, comm
	send_buffer->source = DATA_SOURCE_GO_HTTP2_UPROBE;
	send_buffer->coroutine_id = get_current_goroutine();
	send_buffer->timestamp = bpf_ktime_get_ns();
	bpf_get_current_comm(send_buffer->comm, sizeof(send_buffer->comm));

	// tcp_seq, direction
	int tcp_seq;
	enum traffic_direction direction;
	if (data->read) {
		tcp_seq = get_tcp_read_seq_from_fd(data->fd);
		tcp_seq = get_previous_read_tcp_seq(data->fd, tcp_seq);
		direction = T_INGRESS;
	} else {
		tcp_seq = get_tcp_write_seq_from_fd(data->fd);
		direction = T_EGRESS;
	}

	if (tcp_seq == 0) {
		return;
	}

	send_buffer->tcp_seq = tcp_seq;
	send_buffer->direction = direction;

	// data_type
	enum traffic_protocol protocol;
	if (is_http2_tls()) {
		protocol = PROTO_TLS_HTTP2;
	} else {
		protocol = PROTO_HTTP2;
	}

	send_buffer->data_type = protocol;

	// Refer to the logic of process_data in socket_trace.c to
	// obtain quintuple information
	__u64 id = bpf_get_current_pid_tgid();
	__u32 tgid = id >> 32;
	__u32 k0 = 0;
	struct member_fields_offset *offset = members_offset__lookup(&k0);
	if (!offset)
		return;

	if (unlikely(!offset->ready))
		return;

	send_buffer->tuple.l4_protocol = IPPROTO_TCP;
	void *sk = get_socket_from_fd(data->fd, offset);

	// fill in the port number
	__be16 inet_dport;
	__u16 inet_sport;
	__u16 skc_family;
	struct skc_flags_t {
		unsigned char skc_reuse : 4;
		unsigned char skc_reuseport : 1;
		unsigned char skc_ipv6only : 1;
		unsigned char skc_net_refcnt : 1;
	};
	struct skc_flags_t skc_flags;
	bpf_probe_read(&skc_flags, sizeof(skc_flags),
		       sk + STRUCT_SOCK_COMMON_IPV6ONLY_OFFSET);
	bpf_probe_read(&skc_family, sizeof(skc_family),
		       sk + STRUCT_SOCK_FAMILY_OFFSET);
	bpf_probe_read(&inet_dport, sizeof(inet_dport),
		       sk + STRUCT_SOCK_DPORT_OFFSET);
	bpf_probe_read(&inet_sport, sizeof(inet_sport),
		       sk + STRUCT_SOCK_SPORT_OFFSET);
	send_buffer->tuple.dport = __bpf_ntohs(inet_dport);
	send_buffer->tuple.num = inet_sport;

	switch (skc_family) {
	case PF_INET:
		bpf_probe_read(send_buffer->tuple.rcv_saddr, 4,
			       sk + STRUCT_SOCK_SADDR_OFFSET);
		bpf_probe_read(send_buffer->tuple.daddr, 4,
			       sk + STRUCT_SOCK_DADDR_OFFSET);
		send_buffer->tuple.addr_len = 4;
		break;
	case PF_INET6:
		bpf_probe_read(send_buffer->tuple.rcv_saddr, 16,
			       sk + STRUCT_SOCK_IP6SADDR_OFFSET);
		bpf_probe_read(send_buffer->tuple.daddr, 16,
			       sk + STRUCT_SOCK_IP6SADDR_OFFSET);
		send_buffer->tuple.addr_len = 16;
		break;
	}


	// trace_uid, generator for socket_id
	struct trace_uid_t *trace_uid = trace_uid_map__lookup(&k0);
	if (trace_uid == NULL)
		return;

	// Update and get socket_id
	__u64 conn_key;
	struct socket_info_t *socket_info_ptr;
	conn_key = gen_conn_key_id((__u64)tgid, (__u64)data->fd);
	socket_info_ptr = socket_info_map__lookup(&conn_key);
	if (is_socket_info_valid(socket_info_ptr)) {
		send_buffer->socket_id = socket_info_ptr->uid;
	} else {
		send_buffer->socket_id = trace_uid->socket_id + 1;
		trace_uid->socket_id++;

		struct socket_info_t sk_info = {
			.uid = send_buffer->socket_id,
		};
		socket_info_map__update(&conn_key, &sk_info);
		struct trace_stats *trace_stats = trace_stats_map__lookup(&k0);
		if (trace_stats == NULL)
			return;
		trace_stats->socket_map_count++;
	}

	send_buffer->tgid = tgid;
	send_buffer->pid = (__u32)id;
}

// 填充 buffer->send_buffer.data
static __inline void
http2_fill_buffer_and_send(struct http2_header_data *data,
			   struct __http2_buffer *buffer,
			   struct __socket_data *send_buffer)
{
	if (!data || !buffer || !send_buffer || !send_buffer->tcp_seq) {
		return;
	}
	send_buffer->msg_type = data->message_type;

	buffer->fd = data->fd;
	buffer->stream_id = data->stream;
	buffer->header_len = data->name.len & 0x03FF;
	buffer->value_len = data->value.len & 0x03FF;

	__u32 count = 16 + buffer->header_len + buffer->value_len;
	if (count > HTTP2_BUFFER_INFO_SIZE)
		return;
	send_buffer->syscall_len = count;
	send_buffer->data_len = count;
	// Useless range  checking. Make the eBPF validator happy
	if (buffer->header_len >= 0) {
		if (buffer->header_len < HTTP2_BUFFER_INFO_SIZE) {
			bpf_probe_read(buffer->info, 1 + buffer->header_len,
				       data->name.ptr);
		}
	}

	// Useless range  checking. Make the eBPF validator happy
	if (buffer->header_len >= 0) {
		if (buffer->header_len < HTTP2_BUFFER_INFO_SIZE) {
			if (buffer->value_len < HTTP2_BUFFER_INFO_SIZE) {
				bpf_probe_read(
					buffer->info + buffer->header_len,
					1 + buffer->value_len, data->value.ptr);
			}
		}
	}
	if (buffer->header_len + buffer->value_len < HTTP2_BUFFER_INFO_SIZE) {
		buffer->info[buffer->header_len + buffer->value_len] = 0;
	}

	report_http2_header(data->ctx);
}

struct http2_headers_data {
	bool read : 1;
	int fd;
	struct go_slice *fields;
	__u32 stream;
	enum message_type message_type;
	struct pt_regs *ctx;
};

// Send multiple header messages and add an end marker message at the end
static __inline int submit_http2_headers(struct http2_headers_data *headers)
{
	struct http2_header_data data = {
		.read = headers->read,
		.fd = headers->fd,
		.stream = headers->stream,
		.message_type = headers->message_type,
		.ctx = headers->ctx,
	};
	struct __http2_stack *stack = get_http2_stack();
	if(!stack){
		return 0;
	}

	struct __http2_buffer *buffer = &(stack->http2_buffer);
	struct __socket_data *send_buffer = &(stack->send_buffer);

	http2_fill_common_socket(&data, send_buffer);

	int idx;
	struct go_http2_header_field *tmp;
	struct go_http2_header_field field;

#pragma unroll
	for (idx = 0; idx < 9; ++idx) {
		if (idx >= headers->fields->len)
			break;

		tmp = headers->fields->ptr;
		bpf_probe_read(&field, sizeof(field), tmp + idx);
		data.name = field.name;
		data.value = field.value;
		http2_fill_buffer_and_send(&data, buffer, send_buffer);
	}

	data.name.len = 0;
	data.value.len = 0;

	// MSG_REQUEST -> MSG_REQUEST_END
	// MSG_RESPONSE -> MSG_RESPONSE_END
	data.message_type += 2;

	http2_fill_buffer_and_send(&data, buffer, send_buffer);
	return 0;
}

static __inline __u32
get_stream_from_http2MetaHeadersFrame(void *ptr, struct ebpf_proc_info *info)
{
	bpf_probe_read(&ptr, sizeof(ptr), ptr);
	ptr += info->offsets[OFFSET_IDX_STREAM_ID_HTTP2_FRAME_HEADER];
	__u32 stream;
	bpf_probe_read(&stream, sizeof(stream), ptr);
	return stream;
}

static __inline void *
get_fields_from_http2MetaHeadersFrame(void *ptr, struct ebpf_proc_info *info)
{
	ptr += info->offsets[OFFSET_IDX_FIELDS_HTTP2_META_HEADERS_FRAME];
	return ptr;
}

// func (cc *http2ClientConn) writeHeader(name, value string)
SEC("uprobe/go_http2ClientConn_writeHeader")
int uprobe_go_http2ClientConn_writeHeader(struct pt_regs *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	struct http2_header_data data = {
		.read = false,
		.fd = get_fd_from_http2ClientConn_ctx(ctx, info),
		.message_type = MSG_REQUEST,
		.ctx = ctx,
	};

	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_STREAM_HTTP2_CLIENT_CONN];
	bpf_probe_read(&(data.stream), sizeof(data.stream), ptr);
	data.stream -= 2;

	struct __http2_stack *stack = get_http2_stack();
	if(!stack){
		return 0;
	}

	struct __http2_buffer *buffer = &(stack->http2_buffer);
	struct __socket_data *send_buffer = &(stack->send_buffer);

	http2_fill_common_socket(&data, send_buffer);

	if (info->version >= GO_VERSION(1, 17, 0)) {
		data.name.ptr = (void *)ctx->rbx;
		data.name.len = ctx->rcx;
		data.value.ptr = (void *)ctx->rdi;
		data.value.len = ctx->rsi;
	} else {
		bpf_probe_read(&data.name.ptr, sizeof(data.name.ptr),
			       (void *)(ctx->rsp + 16));
		bpf_probe_read(&data.name.len, sizeof(data.name.len),
			       (void *)(ctx->rsp + 24));
		bpf_probe_read(&data.value.ptr, sizeof(data.value.ptr),
			       (void *)(ctx->rsp + 32));
		bpf_probe_read(&data.value.len, sizeof(data.value.len),
			       (void *)(ctx->rsp + 40));
	}
	http2_fill_buffer_and_send(&data, buffer, send_buffer);
	return 0;
}

// func (cc *http2ClientConn) writeHeaders(streamID uint32, endStream bool, maxFrameSize int, hdrs []byte) error
SEC("uprobe/go_http2ClientConn_writeHeaders")
int uprobe_go_http2ClientConn_writeHeaders(struct pt_regs *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	struct http2_header_data data = {};
	void *ptr = get_the_first_parameter(ctx, info);
	ptr += info->offsets[OFFSET_IDX_STREAM_HTTP2_CLIENT_CONN];
	bpf_probe_read(&(data.stream), sizeof(data.stream), ptr);
	data.stream -= 2;
	data.read = false;
	data.fd = get_fd_from_http2ClientConn_ctx(ctx, info);
	data.message_type = MSG_REQUEST_END;
	data.ctx = ctx;
	data.name.len = 0;
	data.value.len = 0;

	struct __http2_stack *stack = get_http2_stack();
	if(!stack){
		return 0;
	}

	struct __http2_buffer *buffer = &(stack->http2_buffer);
	struct __socket_data *send_buffer = &(stack->send_buffer);

	http2_fill_common_socket(&data, send_buffer);
	http2_fill_buffer_and_send(&data, buffer, send_buffer);
	return 0;
}

// func (sc *http2serverConn) processHeaders(f *http2MetaHeadersFrame) error
SEC("uprobe/go_http2serverConn_processHeaders")
int uprobe_go_http2serverConn_processHeaders(struct pt_regs *ctx)
{
	struct go_slice fields;
	void *frame;
	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}
	if (info->version >= GO_VERSION(1, 17, 0)) {
		frame = (void *)ctx->rbx;
	} else {
		bpf_probe_read(&frame, sizeof(frame), (void *)(ctx->rsp + 16));
	}

	void *fields_ptr = get_fields_from_http2MetaHeadersFrame(frame, info);
	bpf_probe_read(&fields, sizeof(fields), fields_ptr);

	struct http2_headers_data headers = {
		.fields = &fields,
		.read = true,
		.fd = get_fd_from_http2serverConn_ctx(ctx, info),
		.stream = get_stream_from_http2MetaHeadersFrame(frame, info),
		.message_type = MSG_REQUEST,
		.ctx = ctx,
	};

	return submit_http2_headers(&headers);
}

// func (sc *http2serverConn) writeHeaders(st *http2stream, headerData *http2writeResHeaders) error
SEC("uprobe/go_http2serverConn_writeHeaders")
int uprobe_go_http2serverConn_writeHeaders(struct pt_regs *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	// headerData *http2writeResHeaders
	void *ptr;

	struct http2_header_data data = {};
	data.read = false;
	data.fd = get_fd_from_http2serverConn_ctx(ctx, info);
	data.message_type = MSG_RESPONSE;
	data.ctx = ctx;

	struct __http2_stack *stack = get_http2_stack();
	if(!stack){
		return 0;
	}

	struct __http2_buffer *buffer = &(stack->http2_buffer);
	struct __socket_data *send_buffer = &(stack->send_buffer);

	http2_fill_common_socket(&data, send_buffer);

	if (info->version >= GO_VERSION(1, 17, 0)) {
		ptr = (void *)ctx->rcx;
	} else {
		bpf_probe_read(&ptr, sizeof(ptr), (void *)(ctx->rsp + 24));
	}

	bpf_probe_read(&(data.stream), sizeof(data.stream), ptr + 0x0);

	char status[] = ":status";
	char status_value[3];
	unsigned int code;
	bpf_probe_read(&code, sizeof(code), ptr + 0x8);
	if (code) {
		status_value[0] = '0' + (code % 1000) / 100;
		status_value[1] = '0' + (code % 100) / 10;
		status_value[2] = '0' + (code % 10);
		data.name.ptr = (char *)&status;
		data.name.len = 7;
		data.value.ptr = (char *)&status_value;
		data.value.len = 3;
		http2_fill_buffer_and_send(&data, buffer, send_buffer);
	}

	char date[] = "date";
	data.name.ptr = (char *)&date;
	data.name.len = 4;
	bpf_probe_read(&(data.value), sizeof(data.value), ptr + 0x38);
	if (data.value.len) {
		http2_fill_buffer_and_send(&data, buffer, send_buffer);
	}

	char content_type[] = "content-type";
	data.name.ptr = (char *)&content_type;
	data.name.len = 12;
	bpf_probe_read(&(data.value), sizeof(data.value), ptr + 0x48);
	if (data.value.len) {
		http2_fill_buffer_and_send(&data, buffer, send_buffer);
	}

	char content_length[] = "content-length";
	data.name.ptr = (char *)content_length;
	data.name.len = 14;
	bpf_probe_read(&(data.value), sizeof(data.value), ptr + 0x58);
	if (data.value.len) {
		http2_fill_buffer_and_send(&data, buffer, send_buffer);
	}

	data.name.len = 0;
	data.value.len = 0;
	data.message_type += 2;
	http2_fill_buffer_and_send(&data, buffer, send_buffer);

	return 0;
}

// func (rl *http2clientConnReadLoop) handleResponse(cs *http2clientStream, f *http2MetaHeadersFrame) (*Response, error)
SEC("uprobe/go_http2clientConnReadLoop_handleResponse")
int uprobe_go_http2clientConnReadLoop_handleResponse(struct pt_regs *ctx)
{
	struct go_slice fields;
	void *frame;

	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	if (info->version >= GO_VERSION(1, 17, 0)) {
		frame = (void *)ctx->rcx;
	} else {
		bpf_probe_read(&frame, sizeof(frame), (void *)(ctx->rsp + 24));
	}

	void *fields_ptr = get_fields_from_http2MetaHeadersFrame(frame, info);
	bpf_probe_read(&fields, sizeof(fields), fields_ptr);

	struct http2_headers_data headers = {
		.fields = &fields,
		.read = true,
		.fd = get_fd_from_http2clientConnReadLoop_ctx(ctx, info),
		.stream = get_stream_from_http2MetaHeadersFrame(frame, info),
		.message_type = MSG_RESPONSE,
		.ctx = ctx,
	};
	return submit_http2_headers(&headers);
}

// func (l *loopyWriter) writeHeader(streamID uint32, endStream bool, hf []hpack.HeaderField, onWrite func()) error
SEC("uprobe/go_loopyWriter_writeHeader")
int uprobe_go_loopyWriter_writeHeader(struct pt_regs *ctx)
{
	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	struct go_slice fields = { 0 };
	if (info->version >= GO_VERSION(1, 17, 0)) {
		fields.ptr = (void *)ctx->rdi;
		fields.len = ctx->rsi;
		fields.cap = ctx->r8;
	} else {
		// 8 + 8 + 4 + 4
		bpf_probe_read(&fields, sizeof(fields),
			       (void *)(ctx->rsp + 24));
	}

	struct http2_headers_data headers = { 0 };
	headers.fields = &fields;
	headers.read = false;
	headers.fd = get_fd_from_grpc_loopyWriter(ctx, info);
	headers.ctx = ctx;

	if (info->version >= GO_VERSION(1, 17, 0)) {
		headers.stream = (__u32)ctx->rbx;
	} else {
		bpf_probe_read(&headers.stream, sizeof(headers.stream),
			       (void *)(ctx->rsp + 16));
	}

	int is_server_side = get_side_from_grpc_loopyWriter(ctx, info);
	headers.message_type = is_server_side ? MSG_RESPONSE : MSG_REQUEST;
	return submit_http2_headers(&headers);
}

// func (t *http2Server) operateHeaders(frame *http2.MetaHeadersFrame, handle func(*Stream), traceCtx func(context.Context, string) context.Context) (fatal bool)
SEC("uprobe/go_http2Server_operateHeaders")
int uprobe_go_http2Server_operateHeaders(struct pt_regs *ctx)
{
	struct go_slice fields;
	void *frame;

	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	if (info->version >= GO_VERSION(1, 17, 0)) {
		frame = (void *)ctx->rbx;
	} else {
		bpf_probe_read(&frame, sizeof(frame), (void *)(ctx->rsp + 16));
	}

	void *fields_ptr = get_fields_from_http2MetaHeadersFrame(frame, info);
	bpf_probe_read(&fields, sizeof(fields), fields_ptr);

	struct http2_headers_data headers = {
		.fields = &fields,
		.read = true,
		.fd = get_fd_from_grpc_http2Server_ctx(ctx, info),
		.stream = get_stream_from_http2MetaHeadersFrame(frame, info),
		.message_type = MSG_REQUEST,
		.ctx = ctx,
	};

	return submit_http2_headers(&headers);
}

// func (t *http2Client) operateHeaders(frame *http2.MetaHeadersFrame)
SEC("uprobe/go_http2Client_operateHeaders")
int uprobe_go_http2Client_operateHeaders(struct pt_regs *ctx)
{
	struct go_slice fields;
	void *frame;

	__u64 id = bpf_get_current_pid_tgid();
	pid_t pid = id >> 32;

	struct ebpf_proc_info *info = bpf_map_lookup_elem(&proc_info_map, &pid);
	if (!info) {
		return 0;
	}

	if (info->version >= GO_VERSION(1, 17, 0)) {
		frame = (void *)ctx->rbx;
	} else {
		bpf_probe_read(&frame, sizeof(frame), (void *)(ctx->rsp + 16));
	}

	void *fields_ptr = get_fields_from_http2MetaHeadersFrame(frame, info);
	bpf_probe_read(&fields, sizeof(fields), fields_ptr);

	struct http2_headers_data headers = {
		.fields = &fields,
		.read = true,
		.fd = get_fd_from_grpc_http2Client_ctx(ctx, info),
		.stream = get_stream_from_http2MetaHeadersFrame(frame, info),
		.message_type = MSG_RESPONSE,
		.ctx = ctx,
	};

	return submit_http2_headers(&headers);
}