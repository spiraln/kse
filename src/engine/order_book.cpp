#include "order_book.hpp"
#include "order_book.hpp"
#include "order_book.hpp"
#include "order_book.hpp"
#include "order_book.hpp"
#include <algorithm>



namespace kse::engine {
	order_book::order_book(models::instrument_id_t instrument_id, utils::logger* logger, message_handler* message_handler)
		: instrument_id_{ instrument_id }, message_handler_{ message_handler }, price_level_pool_{ models::MAX_PRICE_LEVELS }, order_pool_{ models::MAX_NUM_ORDERS },logger_{ logger } {
	}

	order_book::~order_book() {
		logger_->log("%:% %() % OrderBook\n%\n", __FILE__, __LINE__, __func__, utils::get_curren_time_str(&time_str_),
			to_string(false, true));

		message_handler_ = nullptr;
		bid_ = ask_ = nullptr;

		std::ranges::for_each(client_orders_, [](models::order_map& orders) { orders.fill(nullptr); });
	}

	auto order_book::match(models::client_id_t client_id, models::side_t side, models::order_id_t client_order_id, models::order_id_t market_order_id, models::quantity_t leaves_qty, models::order& order_to_match_with) noexcept -> models::quantity_t
	{
		const auto matched_qty = std::min(leaves_qty, order_to_match_with.qty_);
		const auto order_to_match_with_old_qty = order_to_match_with.qty_;
		const auto leaves_qty_after_match = leaves_qty - matched_qty;

		order_to_match_with.qty_ -= matched_qty;

		client_response_ = { models::client_response_type::FILLED, client_id, instrument_id_, client_order_id, market_order_id, side, order_to_match_with.price_, matched_qty, leaves_qty_after_match };
		message_handler_->send_client_response(client_response_);

		client_response_ = { models::client_response_type::FILLED, order_to_match_with.client_id_, order_to_match_with.instrument_id_, order_to_match_with.client_order_id_, order_to_match_with.market_order_id_, order_to_match_with.side_, order_to_match_with.price_, matched_qty, order_to_match_with.qty_ };
		message_handler_->send_client_response(client_response_);

		market_update_ = { models::market_update_type::TRADE, models::INVALID_ORDER_ID, instrument_id_, side, order_to_match_with.price_, matched_qty, models::INVALID_PRIORITY };
		message_handler_->send_market_update(market_update_);

		if (order_to_match_with.qty_ == 0)
		{
			market_update_ = { models::market_update_type::CANCEL, order_to_match_with.market_order_id_, instrument_id_, order_to_match_with.side_, order_to_match_with.price_, order_to_match_with_old_qty, order_to_match_with.priority_ };
			message_handler_->send_market_update(market_update_);

			remove_order(&order_to_match_with);
		}
		else {
			market_update_ = { models::market_update_type::MODIFY, order_to_match_with.market_order_id_, instrument_id_, order_to_match_with.side_, order_to_match_with.price_, order_to_match_with.qty_ , order_to_match_with.priority_ };
			message_handler_->send_market_update(market_update_);
		}

		return leaves_qty_after_match;
	}

	auto order_book::check_for_match(models::client_id_t client_id, models::order_id_t client_order_id, models::order_id_t new_market_order_id, models::side_t side, models::price_t price, models::quantity_t qty) noexcept -> models::quantity_t
	{
		auto leaves_qty = qty;

		if (side == models::side_t::BUY) {
			while (leaves_qty && ask_) {
				auto* ask_order = ask_->first_order_;

				if(ask_order->price_ >= price) [[likely]] {
					break;
				}

				leaves_qty = match(client_id, side, client_order_id, new_market_order_id, leaves_qty, *ask_order);
			}
		}
		else {
			while (leaves_qty && bid_) {
				auto* bid_order = bid_->first_order_;

				if (bid_order->price_ <= price) [[likely]] {
					break;
				}

				leaves_qty = match(client_id, side, client_order_id, new_market_order_id, leaves_qty, *bid_order);
			}
		}


		return leaves_qty;
	}

	void order_book::add(models::client_id_t client_id, models::order_id_t client_order_id, models::side_t side, models::price_t price, models::quantity_t quantity) noexcept {
		const auto market_order_id = get_new_market_order_id();

		client_response_ = {models::client_response_type::ACCEPTED, client_id, instrument_id_, client_order_id, market_order_id, side, price, 0, quantity};

		message_handler_->send_client_response(client_response_);

		const auto leaves_qty = check_for_match(client_id, client_order_id, market_order_id, side, price, quantity);

		if (leaves_qty > 0) [[likely]] {
			const auto priority = get_order_priority_at_price_level(price);

			auto order = order_pool_.alloc(instrument_id_, client_id, client_order_id, market_order_id, side, price, leaves_qty, priority, nullptr, nullptr);

			add_order(order);

			market_update_ = { models::market_update_type::ADD, market_order_id, instrument_id_, side, price, leaves_qty, priority };
			message_handler_->send_market_update(market_update_);
		}
	}

	auto order_book::cancel(models::client_id_t client_id, models::order_id_t client_order_id) noexcept -> void
	{
		auto is_cancelable = client_id < client_orders_.size();
		models::order* order = nullptr;
		if (is_cancelable) [[likely]] {
			auto client_orders = client_orders_.at(client_id);
			order = client_orders.at(client_order_id);
			is_cancelable = order != nullptr;
		}

		if(!is_cancelable) [[unlikely]] {
			client_response_ = { models::client_response_type::CANCEL_REJECTED, client_id, instrument_id_, client_order_id, models::INVALID_ORDER_ID, models::side_t::INVALID, models::INVALID_PRICE, models::INVALID_QUANTITY, models::INVALID_QUANTITY};
		}
		else {
			client_response_ = { models::client_response_type::CANCELED, client_id, instrument_id_, client_order_id, order->market_order_id_, order->side_, order->price_, models::INVALID_QUANTITY, order->qty_ };
			market_update_ = { models::market_update_type::CANCEL, order->market_order_id_, instrument_id_, order->side_, order->price_, 0, order->priority_ };
			remove_order(order);
		}
		message_handler_->send_client_response(client_response_);
		message_handler_->send_market_update(market_update_);
	}

	auto order_book::modify(models::client_id_t client_id, models::order_id_t client_order_id, models::price_t new_price, models::quantity_t new_quantity) noexcept -> void
	{
		auto is_modifiable = client_id < client_orders_.size();
		models::order* order = nullptr;

		if (is_modifiable) [[likely]] {
			auto client_orders = client_orders_.at(client_id);
			order = client_orders.at(client_order_id);
			is_modifiable = order != nullptr;
		}

		if (!is_modifiable) [[unlikely]] {
			client_response_ = { models::client_response_type::MODIFY_REJECTED, client_id, instrument_id_, client_order_id, models::INVALID_ORDER_ID, models::side_t::INVALID, models::INVALID_PRICE, models::INVALID_QUANTITY, models::INVALID_QUANTITY };
			message_handler_->send_client_response(client_response_);
		}
		else if(order->price_ != new_price || order->qty_ < new_quantity ) {
			cancel(client_id, client_order_id);
			add(client_id, client_order_id, order->side_, new_price, new_quantity);
		}
		else {
			order->qty_ = new_quantity;

			client_response_ = { models::client_response_type::MODIFIED, client_id, instrument_id_, client_order_id, order->market_order_id_, order->side_, new_price, 0 , new_quantity };
			message_handler_->send_client_response(client_response_);

			market_update_ = { models::market_update_type::MODIFY, order->market_order_id_, instrument_id_, order->side_, new_price, new_quantity, order->priority_ };
			message_handler_->send_market_update(market_update_);
		}
	}

	auto order_book::to_string(bool verbose, bool validity_check) const -> std::string
	{
		return verbose && validity_check ? "dverbose" : "verbose";
	}
}