#include "osd_ops.h"
#include "pg_states.h"
#include "etcd_state_client.h"
#include "http_client.h"
#include "base64.h"

json_kv_t etcd_state_client_t::parse_etcd_kv(const json11::Json & kv_json)
{
    json_kv_t kv;
    kv.key = base64_decode(kv_json["key"].string_value());
    std::string json_err, json_text = base64_decode(kv_json["value"].string_value());
    kv.value = json_text == "" ? json11::Json() : json11::Json::parse(json_text, json_err);
    if (json_err != "")
    {
        printf("Bad JSON in etcd key %s: %s (value: %s)\n", kv.key.c_str(), json_err.c_str(), json_text.c_str());
        kv.key = "";
    }
    return kv;
}

void etcd_state_client_t::etcd_call(std::string api, json11::Json payload, int timeout, std::function<void(std::string, json11::Json)> callback)
{
    std::string req = payload.dump();
    req = "POST "+etcd_api_path+api+" HTTP/1.1\r\n"
        "Host: "+etcd_address+"\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: "+std::to_string(req.size())+"\r\n"
        "Connection: close\r\n"
        "\r\n"+req;
    http_request_json(tfd, etcd_address, req, timeout, callback);
}

void etcd_state_client_t::etcd_txn(json11::Json txn, int timeout, std::function<void(std::string, json11::Json)> callback)
{
    etcd_call("/kv/txn", txn, timeout, callback);
}

void etcd_state_client_t::start_etcd_watcher()
{
    etcd_watches_initialised = 0;
    etcd_watch_ws = open_websocket(tfd, etcd_address, etcd_api_path+"/watch", ETCD_SLOW_TIMEOUT, [this](const http_response_t *msg)
    {
        if (msg->body.length())
        {
            std::string json_err;
            json11::Json data = json11::Json::parse(msg->body, json_err);
            if (json_err != "")
            {
                printf("Bad JSON in etcd event: %s, ignoring event\n", json_err.c_str());
            }
            else
            {
                if (data["result"]["created"].bool_value())
                {
                    etcd_watches_initialised++;
                }
                if (etcd_watches_initialised == 4)
                {
                    etcd_watch_revision = data["result"]["header"]["revision"].uint64_value();
                }
                // First gather all changes into a hash to remove multiple overwrites
                json11::Json::object changes;
                for (auto & ev: data["result"]["events"].array_items())
                {
                    auto kv = parse_etcd_kv(ev["kv"]);
                    if (kv.key != "")
                    {
                        changes[kv.key] = kv.value;
                    }
                }
                for (auto & kv: changes)
                {
                    if (this->log_level > 0)
                    {
                        printf("Incoming event: %s -> %s\n", kv.first.c_str(), kv.second.dump().c_str());
                    }
                    parse_state(kv.first, kv.second);
                }
                // React to changes
                on_change_hook(changes);
            }
        }
        if (msg->eof)
        {
            etcd_watch_ws = NULL;
            if (etcd_watches_initialised == 0)
            {
                // Connection not established, retry in <ETCD_SLOW_TIMEOUT>
                tfd->set_timer(ETCD_SLOW_TIMEOUT, false, [this](int)
                {
                    start_etcd_watcher();
                });
            }
            else
            {
                // Connection was live, retry immediately
                start_etcd_watcher();
            }
        }
    });
    etcd_watch_ws->post_message(WS_TEXT, json11::Json(json11::Json::object {
        { "create_request", json11::Json::object {
            { "key", base64_encode(etcd_prefix+"/config/") },
            { "range_end", base64_encode(etcd_prefix+"/config0") },
            { "start_revision", etcd_watch_revision+1 },
            { "watch_id", ETCD_CONFIG_WATCH_ID },
        } }
    }).dump());
    etcd_watch_ws->post_message(WS_TEXT, json11::Json(json11::Json::object {
        { "create_request", json11::Json::object {
            { "key", base64_encode(etcd_prefix+"/osd/state/") },
            { "range_end", base64_encode(etcd_prefix+"/osd/state0") },
            { "start_revision", etcd_watch_revision+1 },
            { "watch_id", ETCD_OSD_STATE_WATCH_ID },
        } }
    }).dump());
    etcd_watch_ws->post_message(WS_TEXT, json11::Json(json11::Json::object {
        { "create_request", json11::Json::object {
            { "key", base64_encode(etcd_prefix+"/pg/state/") },
            { "range_end", base64_encode(etcd_prefix+"/pg/state0") },
            { "start_revision", etcd_watch_revision+1 },
            { "watch_id", ETCD_PG_STATE_WATCH_ID },
        } }
    }).dump());
    etcd_watch_ws->post_message(WS_TEXT, json11::Json(json11::Json::object {
        { "create_request", json11::Json::object {
            { "key", base64_encode(etcd_prefix+"/pg/history/") },
            { "range_end", base64_encode(etcd_prefix+"/pg/history0") },
            { "start_revision", etcd_watch_revision+1 },
            { "watch_id", ETCD_PG_HISTORY_WATCH_ID },
        } }
    }).dump());
}

void etcd_state_client_t::load_global_config()
{
    etcd_call("/kv/range", json11::Json::object {
        { "key", base64_encode(etcd_prefix+"/config/global") }
    }, ETCD_SLOW_TIMEOUT, [this](std::string err, json11::Json data)
    {
        if (err != "")
        {
            printf("Error reading OSD configuration from etcd: %s\n", err.c_str());
            tfd->set_timer(ETCD_SLOW_TIMEOUT, false, [this](int timer_id)
            {
                load_global_config();
            });
            return;
        }
        if (!etcd_watch_revision)
        {
            etcd_watch_revision = data["header"]["revision"].uint64_value();
        }
        json11::Json::object global_config;
        if (data["kvs"].array_items().size() > 0)
        {
            auto kv = parse_etcd_kv(data["kvs"][0]);
            if (kv.value.is_object())
            {
                global_config = kv.value.object_items();
            }
        }
        on_load_config_hook(global_config);
    });
}

void etcd_state_client_t::load_pgs()
{
    json11::Json::array txn = {
        json11::Json::object {
            { "request_range", json11::Json::object {
                { "key", base64_encode(etcd_prefix+"/config/pgs") },
            } }
        },
        json11::Json::object {
            { "request_range", json11::Json::object {
                { "key", base64_encode(etcd_prefix+"/pg/history/") },
                { "range_end", base64_encode(etcd_prefix+"/pg/history0") },
            } }
        },
        json11::Json::object {
            { "request_range", json11::Json::object {
                { "key", base64_encode(etcd_prefix+"/pg/state/") },
                { "range_end", base64_encode(etcd_prefix+"/pg/state0") },
            } }
        },
        json11::Json::object {
            { "request_range", json11::Json::object {
                { "key", base64_encode(etcd_prefix+"/osd/state/") },
                { "range_end", base64_encode(etcd_prefix+"/osd/state0") },
            } }
        },
    };
    json11::Json::object req = { { "success", txn } };
    json11::Json checks = load_pgs_checks_hook();
    if (checks.array_items().size() > 0)
    {
        req["compare"] = checks;
    }
    etcd_txn(req, ETCD_SLOW_TIMEOUT, [this](std::string err, json11::Json data)
    {
        if (err != "")
        {
            printf("Error loading PGs from etcd: %s\n", err.c_str());
            tfd->set_timer(ETCD_SLOW_TIMEOUT, false, [this](int timer_id)
            {
                load_pgs();
            });
            return;
        }
        if (!data["succeeded"].bool_value())
        {
            on_load_pgs_hook(false);
            return;
        }
        for (auto & res: data["responses"].array_items())
        {
            for (auto & kv_json: res["response_range"]["kvs"].array_items())
            {
                auto kv = parse_etcd_kv(kv_json);
                parse_state(kv.key, kv.value);
            }
        }
        on_load_pgs_hook(true);
    });
}

void etcd_state_client_t::parse_state(const std::string & key, const json11::Json & value)
{
    if (key == etcd_prefix+"/config/pgs")
    {
        for (auto & pg_item: this->pg_config)
        {
            pg_item.second.exists = false;
        }
        for (auto & pg_item: value["items"].object_items())
        {
            pg_num_t pg_num = stoull_full(pg_item.first);
            if (!pg_num)
            {
                printf("Bad key in PG configuration: %s (must be a number), skipped\n", pg_item.first.c_str());
                continue;
            }
            this->pg_config[pg_num].exists = true;
            this->pg_config[pg_num].pause = pg_item.second["pause"].bool_value();
            this->pg_config[pg_num].primary = pg_item.second["primary"].uint64_value();
            this->pg_config[pg_num].target_set.clear();
            for (auto pg_osd: pg_item.second["osd_set"].array_items())
            {
                this->pg_config[pg_num].target_set.push_back(pg_osd.uint64_value());
            }
            if (this->pg_config[pg_num].target_set.size() != 3)
            {
                printf("Bad PG %u config format: incorrect osd_set = %s\n", pg_num, pg_item.second["osd_set"].dump().c_str());
                this->pg_config[pg_num].target_set.resize(3);
                this->pg_config[pg_num].pause = true;
            }
        }
    }
    else if (key.substr(0, etcd_prefix.length()+12) == etcd_prefix+"/pg/history/")
    {
        // <etcd_prefix>/pg/history/%d
        pg_num_t pg_num = stoull_full(key.substr(etcd_prefix.length()+12));
        if (!pg_num)
        {
            printf("Bad etcd key %s, ignoring\n", key.c_str());
        }
        else
        {
            auto & pg_cfg = this->pg_config[pg_num];
            pg_cfg.target_history.clear();
            pg_cfg.all_peers.clear();
            // Refuse to start PG if any set of the <osd_sets> has no live OSDs
            for (auto hist_item: value["osd_sets"].array_items())
            {
                std::vector<osd_num_t> history_set;
                for (auto pg_osd: hist_item.array_items())
                {
                    history_set.push_back(pg_osd.uint64_value());
                }
                pg_cfg.target_history.push_back(history_set);
            }
            // Include these additional OSDs when peering the PG
            for (auto pg_osd: value["all_peers"].array_items())
            {
                pg_cfg.all_peers.push_back(pg_osd.uint64_value());
            }
        }
    }
    else if (key.substr(0, etcd_prefix.length()+10) == etcd_prefix+"/pg/state/")
    {
        // <etcd_prefix>/pg/state/%d
        pg_num_t pg_num = stoull_full(key.substr(etcd_prefix.length()+10));
        if (!pg_num)
        {
            printf("Bad etcd key %s, ignoring\n", key.c_str());
        }
        else if (value.is_null())
        {
            this->pg_config[pg_num].cur_primary = 0;
            this->pg_config[pg_num].cur_state = 0;
        }
        else
        {
            osd_num_t cur_primary = value["primary"].uint64_value();
            int state = 0;
            for (auto & e: value["state"].array_items())
            {
                int i;
                for (i = 0; i < pg_state_bit_count; i++)
                {
                    if (e.string_value() == pg_state_names[i])
                    {
                        state = state | pg_state_bits[i];
                        break;
                    }
                }
                if (i >= pg_state_bit_count)
                {
                    printf("Unexpected PG %u state keyword in etcd: %s\n", pg_num, e.dump().c_str());
                    return;
                }
            }
            if (!cur_primary || !value["state"].is_array() || !state ||
                (state & PG_OFFLINE) && state != PG_OFFLINE ||
                (state & PG_PEERING) && state != PG_PEERING ||
                (state & PG_INCOMPLETE) && state != PG_INCOMPLETE)
            {
                printf("Unexpected PG %u state in etcd: primary=%lu, state=%s\n", pg_num, cur_primary, value["state"].dump().c_str());
                return;
            }
            this->pg_config[pg_num].cur_primary = cur_primary;
            this->pg_config[pg_num].cur_state = state;
        }
    }
    else if (key.substr(0, etcd_prefix.length()+11) == etcd_prefix+"/osd/state/")
    {
        // <etcd_prefix>/osd/state/%d
        osd_num_t peer_osd = std::stoull(key.substr(etcd_prefix.length()+11));
        if (peer_osd > 0)
        {
            if (value.is_object() && value["state"] == "up" &&
                value["addresses"].is_array() &&
                value["port"].int64_value() > 0 && value["port"].int64_value() < 65536)
            {
                this->peer_states[peer_osd] = value;
            }
            else
            {
                this->peer_states.erase(peer_osd);
            }
        }
    }
}