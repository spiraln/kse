#include "order_server.hpp"

#include <algorithm>
#include <cstring>



auto kse::server::on_new_connection(uv_stream_t* server, int status) -> void
{
	auto& self = order_server::get_instance();
	self.handle_new_connection(server, status);
}

auto kse::server::order_server::handle_new_connection(uv_stream_t* server, int status) -> void
{
	if (status < 0) [[unlikely]] {
		logger_.log("%:% %() %  new connection error: %\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_), uv_strerror(status));
		return;
	}

	auto conn = std::make_unique<tcp_connection_t>();
	uv_tcp_init(loop_, conn->handle_);
	uv_tcp_nodelay(conn->handle_, 1);

	uv_async_init(loop_, conn->async_write_msg_, write_message);

	conn->async_write_msg_->data = conn.get();


	if (uv_accept(server, (uv_stream_t*)conn->handle_) == 0) {
		logger_.log("%:% %() % have_new_connection for clientID: %\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_), next_client_id_);

		conn->handle_->data = conn.get();

		send_connection_accepted_response(next_client_id_);

		uv_read_start((uv_stream_t*)conn->handle_, alloc_buffer, on_read);

		client_connections_.at(next_client_id_++) = std::move(conn);
	}
	else {
		uv_close((uv_handle_t*)conn->handle_, nullptr);
		logger_.log("%:% %() %  can't establish connection\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_));
	}
}

auto kse::server::alloc_buffer(uv_handle_t* handle, size_t suggested_size [[maybe_unused]], uv_buf_t* buf) -> void
{
	tcp_connection_t* conn = static_cast<tcp_connection_t*>(handle->data);
	buf->base = conn->inbound_data_.data();
	buf->len = static_cast<unsigned long>(conn->inbound_data_.size());
}

auto kse::server::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf [[maybe_unused]] ) -> void
{
	auto& self = order_server::get_instance();
	TIME_MEASURE(T1_OrderServer_TCP_read, self.logger_, self.time_str_);
	tcp_connection_t* conn = static_cast<tcp_connection_t*>(stream->data);

	if (nread < 0) [[unlikely]] {
		if (nread == UV_EOF) {
			self.logger_.log("%:% %() %   connection closed\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&self.time_str_));
		}
		else {
			self.logger_.log("%:% %() %   read error\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&self.time_str_));
		}

		uv_close((uv_handle_t*)stream, nullptr);
	}
	else if (nread > 0) {
		conn->next_rcv_valid_index_ += nread;

		const utils::nananoseconds_t user_time = uv_now(self.loop_) * utils::NANOS_PER_MILLIS;

		self.logger_.debug_log("%:% %() % read socket: len:% utime:% \n", __FILE__, __LINE__, __func__,
			utils::get_curren_time_str(&self.time_str_), conn->next_rcv_valid_index_, user_time);

		self.read_data(conn, user_time);
	}
}

auto kse::server::order_server::read_data(tcp_connection_t* conn, utils::nananoseconds_t user_time) -> void
{
	logger_.log("%:% %() % Received socket:len:% rx:%\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_),
		conn->next_rcv_valid_index_, user_time);

	if (conn->next_rcv_valid_index_ >= sizeof(models::client_request_external)) {
		size_t i = 0;
		for (; i + sizeof(models::client_request_external) <= conn->next_rcv_valid_index_; i += sizeof(models::client_request_external)) {
			START_MEASURE(Exchange_odsDeserialization);
			auto request = deserialize_client_request(conn->inbound_data_.data() + i);
			END_MEASURE(Exchange_odsDeserialization, logger_, time_str_);
			
			logger_.debug_log("%:% %() % Received %\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_), request.to_string());

			if (client_connections_[request.request_.client_id_].get() != conn) [[unlikely]] {
				logger_.debug_log("%:% %() % Invalid socket for this ClientRequest from ClientId:% \n", __FILE__, __LINE__, __func__,
					utils::get_curren_time_str(&time_str_), request.request_.client_id_);
				send_invalid_response(request.request_.client_id_);
				continue;
			}

			auto& next_incoming_seq_num = client_next_incoming_seq_num_[request.request_.client_id_];

			if (request.sequence_number_ != next_incoming_seq_num) [[unlikely]] {
				logger_.debug_log("%:% %() % Incorrect sequence number. ClientId:% SeqNum expected:% received:%\n", __FILE__, __LINE__, __func__,
					utils::get_curren_time_str(&time_str_), request.request_.client_id_, next_incoming_seq_num, request.sequence_number_);
				send_invalid_response(request.request_.client_id_);
				continue;
			}

			++next_incoming_seq_num;

			START_MEASURE(Exchange_FIFOSequencer_addClientRequest);
			fifo_sequencer_.add_request(user_time, request.request_);
			END_MEASURE(Exchange_FIFOSequencer_addClientRequest, logger_, time_str_);
		}
		conn->shift_inbound_buffer(i);
	}
}

auto kse::server::order_server::write_to_socket(tcp_connection_t* conn) -> void
{
	for (auto* data_index = conn->write_queue_.get_next_read_element();
		conn->write_queue_.size() && data_index;
		data_index = conn->write_queue_.get_next_read_element()) {
		uv_buf_t buf = uv_buf_init(conn->get_response_buffer(*data_index), static_cast<unsigned int>(sizeof(models::client_response_external)));
		auto* writer = writer_pool_.alloc();

		uv_write(writer, (uv_stream_t*)conn->handle_, &buf, 1, [](uv_write_t* req, int status) {
			auto& self = order_server::get_instance();
			TIME_MEASURE(T6t_OrderServer_TCP_write, self.logger_, self.time_str_);
			if (status < 0) {
				self.logger_.log("%:% %() % error writing data: %\n", __FILE__, __LINE__, __func__,
					utils::get_curren_time_str(&self.time_str_), uv_strerror(status));
				return;
			}
			self.writer_pool_.free(req);
			self.logger_.log("%:% %() % send data to socket\n", __FILE__, __LINE__, __func__,
				utils::get_curren_time_str(&self.time_str_));
		});

		conn->write_queue_.next_read_index();
	}
}

auto kse::server::write_message(uv_async_t* async) -> void
{
	auto& self = order_server::get_instance();
	self.write_to_socket(static_cast<tcp_connection_t*>(async->data));
}

auto kse::server::order_server::process_responses() -> void {
	while (running_) {
		process_responses_helper(server_responses_);
		process_responses_helper(*matching_engine_responses_);
	}
}

auto kse::server::on_check(uv_check_t* req [[maybe_unused]] ) -> void
{
	auto& self = order_server::get_instance();
	if (self.fifo_sequencer_.is_empty()) {
		return;
	}

	START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish);
	self.fifo_sequencer_.sequence_and_publish();
	END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, self.logger_, self.time_str_);
}
