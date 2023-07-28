/*
Copyright 2023 Aleksandr Lobanov


This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

-----------------------------------------------------------------------

This is a utility application that enables you to 
convert your Telegram chats into a corpus of text files.
It is written in C++ and uses TDLib to communicate with Telegram.

Version: 1.0
Author: Aleksandr Lobanov <dev@alobanov.space>

*/

#include <string.h>
#include <iostream>
#include <set>
#include <limits>
#include <thread>
#include <mutex>
#include <map>
#include <filesystem>
#include <atomic>
#include <csignal>
#include <functional>
#include <fstream>
#include <sstream>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>



namespace td_api = td::td_api;

using Object = td_api::Object;
template <class X = Object> using ObjectPtr = td_api::object_ptr<X>;
using AnyObjectPtr = td_api::object_ptr<Object>;


// overloaded
namespace tx {
    template <class... Fs>
    struct overload;

    template <class F>
    struct overload<F> : public F {
        explicit overload(F f) : F(f) {}
    };

    template <class F, class... Fs>
    struct overload<F, Fs...> : public overload<F>, public overload<Fs...> {
        overload(F f, Fs... fs) : overload<F>(f), overload<Fs...>(fs...) {}

        using overload<F>::operator();
        using overload<Fs...>::operator();
    };

    std::string get_thread_id() {
        std::ostringstream ss;
        ss << std::this_thread::get_id();
        return ss.str();
    }

    template<class... Ss>
    std::string print();

    template<class S>
    std::string print(S _x) {  return (std::ostringstream() << _x).str();  };

    template<class S, class... Ss>
    std::string print(const S& _s, const Ss&... _ss) {
        std::ostringstream ss;
        ss << _s << print(_ss...);
        return ss.str();
    }

    template<class... Ss>
    std::string println(const Ss&... _ss) {
        return print(_ss..., "\n");
    }

    template<class... Ss>
    void sprintln(const Ss&... _ss) {
        std::cout << print(_ss..., "\n") << std::flush;
    }
}  // namespace detail

namespace fs = std::filesystem;

namespace creds {
    auto database_directory_ = "tdlib";
    auto use_message_database_ = true;
    auto use_secret_chats_ = true;
    auto api_id_ = 0;   // Put your API ID here
    auto api_hash_ = "";    // Put your API Hash here
    auto system_language_code_ = "en";
    auto device_model_ = "Desktop";
    auto application_version_ = "1.0";
    auto enable_storage_optimizer_ = true;
}


template <class... F>
auto overload(F... f) {
  return tx::overload<F...>(f...);
}


std::string 
TOKEN_CHANGE_SENDER =      "<|cs|>",
TOKEN_MEDIA =              "<|media|>",
TOKEN_AUTHOR =             "<|--me--|>",
TOKEN_MESSAGE_BEGIN =      "<|m|>",
TOKEN_MESSAGE_END =        "<|--m|>",
OUTPUT_DIR_NAME =          "out";

std::size_t
MESSAGES_PER_REQUEST =     100,
PER_CHAT_BUFFER_SIZE =     33554432;

bool
REVERSE_CHAT_ORDER =       true;


class AutoMessageBuffer {
    using message = ObjectPtr<td_api::message>;

    std::size_t
    mxsize_ {0};

    std::vector<message>
    buffer_;

    std::ofstream 
    ofstream_;

    std::int64_t
    user_id_;

    std::int64_t
    prev_user_id_;

    std::string
    f_name_ { "" };

    std::int64_t get_sender_id_(ObjectPtr<td_api::MessageSender>& sender) {
        if (sender && sender->get_id() == td_api::messageSenderUser::ID) {
            return td::move_tl_object_as<td_api::messageSenderUser>(sender)->user_id_;
        } return 0;
    } 

    std::string get_message_body_(message message) {
        if (message->content_->get_id() == td_api::messageText::ID) {
            return td::move_tl_object_as<td_api::messageText>(message->content_)->text_->text_;
        } else return TOKEN_MEDIA;

    }

    void flush_() {
        if (REVERSE_CHAT_ORDER) {
            std::reverse(this->buffer_.begin(), this->buffer_.end());
        }
                   
        for (auto& b : this->buffer_) {
            auto sender_id = this->get_sender_id_(b->sender_id_);

            if (sender_id != this->prev_user_id_) {
                this->ofstream_ << TOKEN_CHANGE_SENDER;
            } 

            this->ofstream_ << TOKEN_MESSAGE_BEGIN 
                << (sender_id == this->user_id_ ? TOKEN_AUTHOR : "")
                << this->get_message_body_(std::move(b))
                << TOKEN_MESSAGE_END;
            
            this->prev_user_id_ = sender_id;
        }

        this->buffer_.clear();
        this->ofstream_.flush();
    }


    public:
    
    void flush() { return this->flush_(); };

    AutoMessageBuffer() = default;

    AutoMessageBuffer(AutoMessageBuffer&&) = default;

    AutoMessageBuffer& operator=(AutoMessageBuffer&&) = default;

    AutoMessageBuffer(
        size_t elem_count, 
        std::ofstream sink,
        std::string f_name,
        std::int64_t user_id
    ) 
    {
        this->ofstream_ = std::move(sink);
        this->mxsize_ = elem_count;
        this->user_id_ = user_id;
        this->f_name_ = f_name;
    }


    void put(message msg_ptr) {
        if (this->buffer_.size() == this->mxsize_) {
            this->flush_();
        }
        this->buffer_.push_back(std::move(msg_ptr));
    }

    
    ~AutoMessageBuffer() {
        this->flush_();
        this->ofstream_.flush();
        this->ofstream_.close();
    }
};


class TelegramHarvestChats {
    using Handler = std::function<void(AnyObjectPtr&, std::int64_t)>;
    using RequestFactory = std::function<ObjectPtr<td_api::Function>(void)>;

    std::unique_ptr< td::ClientManager >
    manager_ = std::make_unique< td::ClientManager >();

    std::int64_t
    client_id_ { 0LL };

    std::int64_t
    user_id_ {};

    std::int64_t 
    current_chat_messages_count_ { 0 };

    std::int64_t
    last_received_message_ { };

    std::vector<std::int64_t>
    chat_ids_ { };

    std::int64_t
    query_id_ { 1LL };

    std::map<std::int64_t, std::pair<RequestFactory, Handler>>
    callbacks_;

    AutoMessageBuffer
    current_buffer_;

    bool
    is_running_ { false },
    is_initialized_ { false };


    void progress_update_() {
        std::cout << "\r" << std::flush;
        // std::cout << "[.. ] " << this->current_chat_messages_count_ << " messages received. Chat -- " << this->chat_ids_.back() << "\r" << std::flush;
        std::cout << "[... ] " << this->chat_ids_.back() << " " << this->current_chat_messages_count_ << " messages received\r" << std::flush;
    }

    void send_request_with_handler_(RequestFactory f, Handler h) {
        auto query_id = this->query_id_++;
        callbacks_.emplace(query_id, std::pair<RequestFactory, Handler>{f, h});
        this->manager_->send(
            this->client_id_,
            query_id,
            std::move(f())
        );
    }


    void send_request_(RequestFactory f) {
        auto query_id = this->query_id_++;
        this->manager_->send(
            this->client_id_,
            query_id,
            std::move(f())
        );
    }


    void on_all_chat_messages_received_(std::int64_t chat_id) {
        tx::sprintln("[OK ] Chat: ", chat_id, "--\t\t total:", current_chat_messages_count_);
        this->chat_ids_.pop_back();
        this->current_chat_messages_count_ = 0;
        this->last_received_message_ = 0;
        this->current_buffer_.flush();

        if (this->chat_ids_.empty()) {
            tx::sprintln("[OK ] All chats are harvested");
            this->is_running_ = false;
            return;
        }


        auto next_chat_id = this->chat_ids_.back();
        fs::remove(fs::path(OUTPUT_DIR_NAME) / fs::path(std::to_string(next_chat_id)));
        this->current_buffer_ = AutoMessageBuffer{ 
            PER_CHAT_BUFFER_SIZE,
            std::ofstream(fs::path(OUTPUT_DIR_NAME) / fs::path(std::to_string(next_chat_id))),
            std::to_string(next_chat_id),
            this->user_id_
        };
    }


    ObjectPtr<td::td_api::Function> 
    message_request_factory_() {
        auto request = td_api::make_object<td_api::getChatHistory>();
        request->chat_id_ = this->chat_ids_.back();
        request->from_message_id_ = last_received_message_;
        request->limit_ = MESSAGES_PER_REQUEST;
        request->only_local_ = false;
        return std::move(request);
    }


    void on_message_received_recursive(AnyObjectPtr& object) {
        if (object == nullptr || object->get_id() != td_api::messages::ID) {  
            if (object->get_id() == td_api::error::ID) {
                auto err = td::move_tl_object_as<td_api::error>(object);
                std::cerr << tx::println("[ERR] ", err->code_, " ", err->message_) << std::flush;
            }  
            this->on_all_chat_messages_received_(this->chat_ids_.back());
        } else {
            auto messages = td::move_tl_object_as<td_api::messages>(object);
            if (messages->total_count_ == 0) {
                this->on_all_chat_messages_received_(this->chat_ids_.back());
            } else {
                this->current_chat_messages_count_ += messages->total_count_;
                this->last_received_message_ = (*messages->messages_.rbegin())->id_;
                this->progress_update_();
                for (auto& msg : messages->messages_) {
                    this->current_buffer_.put(std::move(msg));
                }
            }

        }

        this->send_request_with_handler_(
            [this]() { return this->message_request_factory_(); },
            [this](AnyObjectPtr& _messages, std::int64_t i) { return this->on_message_received_recursive(_messages); }
        );
    }


    void on_chats_received_(ObjectPtr<td_api::chats>& chats) {
        std::sort(
            this->chat_ids_.begin(),
            this->chat_ids_.end(),
            [](auto& a, auto& b) { return a < b; }
        );


        auto last = std::unique(
            this->chat_ids_.begin(),
            this->chat_ids_.end()
        );

        this->chat_ids_.erase(last, this->chat_ids_.end());

        tx::sprintln("[OK ] Total chats: ", this->chat_ids_.size());
        tx::sprintln("[.. ] Retrieving messages");
        fs::remove_all(OUTPUT_DIR_NAME);
        fs::create_directory(OUTPUT_DIR_NAME);
        this->current_buffer_ = AutoMessageBuffer{ 
            PER_CHAT_BUFFER_SIZE,
            std::ofstream(fs::path(OUTPUT_DIR_NAME) / fs::path(std::to_string(this->chat_ids_.back()))),
            std::to_string(this->chat_ids_.back()),
            this->user_id_
        };

        this->send_request_with_handler_(
            [this]() { return this->message_request_factory_(); },
            [this](AnyObjectPtr& _messages, std::int64_t i) { return this->on_message_received_recursive(_messages); }
        );
    }


    void on_auth_state_update_(td_api::updateAuthorizationState& update) {
        td_api::downcast_call(*update.authorization_state_, overload(
            [this](auto&){},
            [this](td_api::authorizationStateWaitPhoneNumber&) {
                std::cout << "Phone: " << std::flush;
                std::string phone;
                std::cin >> phone;
                this->send_request_(
                    [phone](){ 
                        return td_api::make_object<td_api::setAuthenticationPhoneNumber>(phone, nullptr); 
                    }
                );
            },
            [this](td_api::authorizationStateWaitCode&) {
                std::cout << "Enter authentication code: " << std::flush;
                std::string code;
                std::cin >> code;
                this->send_request_(
                    [code]() { 
                        return td_api::make_object<td_api::checkAuthenticationCode>(code);
                    }
                );
            },
            [this](td_api::authorizationStateWaitPassword &) {
                std::cout << "Enter authentication password: " << std::flush;
                std::string password;
                std::cin >> password;
                    this->send_request_(
                    [password]() { 
                        return td_api::make_object<td_api::checkAuthenticationPassword>(password);
                    }
                );
            },
            [this](td_api::authorizationStateWaitTdlibParameters &) {
                auto request = td_api::make_object<td_api::setTdlibParameters>();
                request->database_directory_ = creds::database_directory_;
                request->use_message_database_ = creds::use_message_database_;
                request->use_secret_chats_ = creds::use_secret_chats_;
                request->api_id_ = creds::api_id_;
                request->api_hash_ = creds::api_hash_;
                request->system_language_code_ = creds::system_language_code_;
                request->device_model_ = creds::device_model_;
                request->application_version_ = creds::application_version_;
                request->enable_storage_optimizer_ = creds::enable_storage_optimizer_;
                this->send_request_(
                    [](){ 
                        auto request = td_api::make_object<td_api::setTdlibParameters>();
                        request->database_directory_ = creds::database_directory_;
                        request->use_message_database_ = creds::use_message_database_;
                        request->use_secret_chats_ = creds::use_secret_chats_;
                        request->api_id_ = creds::api_id_;
                        request->api_hash_ = creds::api_hash_;
                        request->system_language_code_ = creds::system_language_code_;
                        request->device_model_ = creds::device_model_;
                        request->application_version_ = creds::application_version_;
                        request->enable_storage_optimizer_ = creds::enable_storage_optimizer_;
                        return std::move(request);
                    }
                );
            },
            [this](td_api::authorizationStateReady &) {
                tx::sprintln("[OK ] Authorization is completed");
                tx::sprintln("[.. ] Getting Telegram ID");
                this->send_request_with_handler_(
                    [](){ return td_api::make_object<td_api::getMe>(); },
                    [this](AnyObjectPtr& _o, auto) {  
                        auto user = td::move_tl_object_as<td_api::user>(_o);
                        this->user_id_ = user->id_;
                        tx::sprintln("[OK ] ID: ", user_id_);
                    }
                );

                tx::sprintln("[.. ] Getting a list of chats");
                this->send_request_with_handler_(
                    [](){ return td_api::make_object<td_api::getChats>(nullptr, std::numeric_limits<std::int32_t>::max()); },
                    [this](AnyObjectPtr& _o, auto) {  
                        auto chats = td::move_tl_object_as<td_api::chats>(_o);
                        for (auto it = chats->chat_ids_.begin();
                            it != chats->chat_ids_.end(); ++it) {
                            this->chat_ids_.push_back(*it);
                        }

                        this->on_chats_received_(chats);
                    }
                );
            }
        ));
    }


    void on_update_(ObjectPtr<td_api::Update>& update){
        td_api::downcast_call(*update, overload(
            [this](td_api::updateAuthorizationState& update) {
                this->on_auth_state_update_(update);
            },
            [this](auto& _o) { }
        ));

    }


    void on_response_(td::ClientManager::Response response) {
        if (response.object == nullptr) 
            return;

        if (response.request_id == 0) {
            auto update = td::move_tl_object_as<td_api::Update>(response.object);
            return this->on_update_(update);
        } else {
            auto it = this->callbacks_.find(response.request_id);
            if (it != this->callbacks_.end()) {
                (*it).second.second(response.object, response.request_id);
                this->callbacks_.erase(it);
            }
        }
    }


    void init_tg_() {
        td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(0));
        this->client_id_ = this->manager_->create_client_id();
        this->send_request_(
            [](){ return td_api::make_object<td_api::getOption>("version"); }
        );
        this->is_initialized_ = true;
    }

public:
    void listen() {
        if (!this->is_initialized_) {
            this->init_tg_();
        }

        this->is_running_ = true;
        while (is_running_) {
            auto response = this->manager_->receive(1);
            if (response.object)
                this->on_response_(std::move(response));
        }
    }
}; // class TelegramHarvestChats;


bool hasarg(char* argv[], const char* arg) {
    for (int i = 0; argv[i] != nullptr; ++i) {
        if (strcmp(argv[i], arg) == 0) {
            return true;
        }
    } return false;
}

const char* getarg(char* argv[], const char* arg) {
    for (int i = 0; argv[i] != nullptr; ++i) {
        if (strcmp(argv[i], arg) == 0) {
            return argv[i + 1];
        }
    } return nullptr;
}


int main(int argc, char* argv[]) {
    if (hasarg(argv, "-h") || hasarg(argv, "--help")) {
        tx::sprintln("Usage: tx_harvestchats [OPTIONS]");
        tx::sprintln("Options:");
        tx::sprintln("\t-h");
        tx::sprintln("\t--help\t\t\t\tShow this help message and exit");
        tx::sprintln("\t--TOKEN_CHANGE_SENDER\t\tToken that is used to separate messages from different senders. Default: <|cs|>");
        tx::sprintln("\t--TOKEN_MEDIA\t\t\tToken that is used to separate media messages. Default: <|media|>");
        tx::sprintln("\t--TOKEN_AUTHOR\t\t\tToken that is used to mark messages from the user. Default: <|--me--|>");
        tx::sprintln("\t--TOKEN_MESSAGE_BEGIN\t\tToken that is used to mark the beginning of a message. Default: <|m|>");
        tx::sprintln("\t--TOKEN_MESSAGE_END\t\tToken that is used to mark the end of a message. Default: <|--m|>");
        tx::sprintln("\t--OUTPUT_DIR_NAME\t\tName of the output directory. Default: out");
        tx::sprintln("\t--MESSAGES_PER_REQUEST\t\tNumber of messages to retrieve per request. Default: 100");
        tx::sprintln("\t--PER_CHAT_BUFFER_SIZE\t\tSize of the buffer for each chat. Default: 33554432");
        tx::sprintln("\t--REVERSE_CHAT_ORDER\t\tReverse the order of messages in each chat. Default: true");
        return 0;
    }

    

    if (hasarg(argv, "--TOKEN_CHANGE_SENDER")) {
        auto __arg = getarg(argv, "--TOKEN_CHANGE_SENDER");
        if (__arg != nullptr) {
            TOKEN_CHANGE_SENDER = __arg;
        } else {
            tx::sprintln("Error: --TOKEN_CHANGE_SENDER requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--TOKEN_MEDIA")) {
        auto __arg = getarg(argv, "--TOKEN_MEDIA");
        if (__arg != nullptr) {
            TOKEN_MEDIA = __arg;
        } else {
            tx::sprintln("Error: --TOKEN_MEDIA requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--TOKEN_AUTHOR")) {
        auto __arg = getarg(argv, "--TOKEN_AUTHOR");
        if (__arg != nullptr) {
            TOKEN_AUTHOR = __arg;
        } else {
            tx::sprintln("Error: --TOKEN_AUTHOR requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--TOKEN_MESSAGE_BEGIN")) {
        auto __arg = getarg(argv, "--TOKEN_MESSAGE_BEGIN");
        if (__arg != nullptr) {
            TOKEN_MESSAGE_BEGIN = __arg;
        } else {
            tx::sprintln("Error: --TOKEN_MESSAGE_BEGIN requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--TOKEN_MESSAGE_END")) {
        auto __arg = getarg(argv, "--TOKEN_MESSAGE_END");
        if (__arg != nullptr) {
            TOKEN_MESSAGE_END = __arg;
        } else {
            tx::sprintln("Error: --TOKEN_MESSAGE_END requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--OUTPUT_DIR_NAME")) {
        auto __arg = getarg(argv, "--OUTPUT_DIR_NAME");
        if (__arg != nullptr) {
            OUTPUT_DIR_NAME = __arg;
        } else {
            tx::sprintln("Error: --OUTPUT_DIR_NAME requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--MESSAGES_PER_REQUEST")) {
        auto __arg = getarg(argv, "--MESSAGES_PER_REQUEST");
        if (__arg != nullptr) {
            try {
                MESSAGES_PER_REQUEST = std::stoi(__arg);
            } catch (...) {
                tx::sprintln("Error: --MESSAGES_PER_REQUEST requires an integer argument");
                return 1;
            }
            
        } else {
            tx::sprintln("Error: --MESSAGES_PER_REQUEST requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--PER_CHAT_BUFFER_SIZE")) {
        auto __arg = getarg(argv, "--PER_CHAT_BUFFER_SIZE");
        if (__arg != nullptr) {
            try {
                PER_CHAT_BUFFER_SIZE = std::stoi(__arg);
            } catch (...) {
                tx::sprintln("Error: --PER_CHAT_BUFFER_SIZE requires an integer argument");
                return 1;
            }
            
        } else {
            tx::sprintln("Error: --PER_CHAT_BUFFER_SIZE requires an argument");
            return 1;
        }
    }


    if (hasarg(argv, "--REVERSE_CHAT_ORDER")) {
        auto __arg = getarg(argv, "--REVERSE_CHAT_ORDER");
        if (__arg != nullptr) {
            if (strcmp(__arg, "true") == 0) {
                REVERSE_CHAT_ORDER = true;
            } else if (strcmp(__arg, "false") == 0) {
                REVERSE_CHAT_ORDER = false;
            } else {
                tx::sprintln("Error: --REVERSE_CHAT_ORDER requires a boolean argument");
                return 1;
            }
        } else {
            tx::sprintln("Error: --REVERSE_CHAT_ORDER requires an argument");
            return 1;
        }
    }


    tx::sprintln("\n\n Running with the following parameters:\t");
    tx::sprintln("\tTOKEN_CHANGE_SENDER:\t", TOKEN_CHANGE_SENDER);
    tx::sprintln("\tTOKEN_MEDIA:\t", TOKEN_MEDIA);
    tx::sprintln("\tTOKEN_AUTHOR:\t", TOKEN_AUTHOR);
    tx::sprintln("\tTOKEN_MESSAGE_BEGIN:\t", TOKEN_MESSAGE_BEGIN);
    tx::sprintln("\tTOKEN_MESSAGE_END:\t", TOKEN_MESSAGE_END);
    tx::sprintln("\tOUTPUT_DIR_NAME:\t", OUTPUT_DIR_NAME);
    tx::sprintln("\tMESSAGES_PER_REQUEST:\t", MESSAGES_PER_REQUEST);
    tx::sprintln("\tPER_CHAT_BUFFER_SIZE:\t", PER_CHAT_BUFFER_SIZE);
    tx::sprintln("\tREVERSE_CHAT_ORDER:\t", REVERSE_CHAT_ORDER);
    tx::sprintln("\n\n");


    auto app = TelegramHarvestChats();
    app.listen();

    return 0;
}