// Copyright wkl04 2019
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#if !defined(MQTT_RETAINED_TOPIC_MAP_HPP)
#define MQTT_RETAINED_TOPIC_MAP_HPP

#include <map>


#include <mqtt/string_view.hpp>
#include <mqtt/optional.hpp>

#include <boost/optional.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include "topic_filter_tokenizer.hpp"

template<typename Value>
class retained_topic_map {
    using node_id_t = std::size_t;
    using path_entry_key = std::pair<node_id_t, MQTT_NS::buffer>;

    static constexpr node_id_t root_node_id = 0;

    struct path_entry {
        node_id_t id;
        std::size_t count = 1;

        static constexpr std::size_t max_count = std::numeric_limits<std::size_t>::max();

        MQTT_NS::optional<Value> value;

        path_entry(node_id_t _id)
            : id(_id)
        { }
    };

    using map_type = std::map< path_entry_key, path_entry >;
    using map_type_iterator = typename map_type::iterator;
    using map_type_const_iterator = typename map_type::const_iterator;

    map_type map;
    size_t map_size = 0;
    map_type_iterator root;
    node_id_t next_node_id = root_node_id;

    map_type_iterator create_topic(MQTT_NS::string_view topic) {
        map_type_iterator parent = root;

        // Check on root entry if we can still add an entry
        if (parent->second.count == path_entry::max_count) {
            throw std::overflow_error("Maximum number of topics reached");
        }

        topic_filter_tokenizer(
            topic,
            [this, &parent](MQTT_NS::string_view t) {
                if (t == "+" || t == "#") {
                    throw std::runtime_error("No wildcards allowed in retained topic name");
                }

                node_id_t parent_id = parent->second.id;
                map_type_iterator entry = map.find(path_entry_key(parent_id, t));

                if (entry == map.end()) {
                    entry = map.emplace(path_entry_key(parent_id, MQTT_NS::allocate_buffer(t)), path_entry(next_node_id++)).first;
                    if (next_node_id == std::numeric_limits<node_id_t>::max()) {
                        throw std::overflow_error("Maximum number of topics reached");
                    }
                }
                else {
                    entry->second.count++;
                }

                parent = entry;
                return true;
            }
        );

        return parent;
    }

    std::vector<map_type_iterator> find_topic(MQTT_NS::string_view topic) {
        std::vector<map_type_iterator> path;
        map_type_iterator parent = root;

        topic_filter_tokenizer(
            topic,
            [this, &parent, &path](MQTT_NS::string_view t) {
                map_type_iterator entry = map.find(path_entry_key(parent->second.id, t));

                if (entry == map.end()) {
                    path = std::vector<map_type_iterator>();
                    return false;
                }

                path.push_back(entry);
                parent = entry;
                return true;
            }
        );

        return path;
    }

    // Match all underlying topics when a hash entry is matched
    // perform a breadth-first iteration over all items in the tree below
    template<typename Output>
    void match_hash_entries(node_id_t parent, Output callback, bool ignore_system) const {
        std::deque<node_id_t> entries;
        entries.push_back(parent);
        std::deque<node_id_t> new_entries;

        while (!entries.empty()) {
            new_entries.resize(0);

            for (auto root : entries) {
                // Find all entries below this node
                for (map_type_const_iterator i = map.lower_bound(path_entry_key(root, MQTT_NS::string_view(""))); i != map.end(); ++i) {
                    if (i->first.first != root) {
                        break;
                    }

                    // Should we ignore system matches
                    if (!ignore_system || (i->first.second.empty() ? true : i->first.second[0] != '$')) {
                        if (i->second.value) {
                            callback(*i->second.value);
                        }

                        new_entries.push_back(i->second.id);
                    }
                }
            }

            // Ignore system only on first level
            ignore_system = false;
            std::swap(entries, new_entries);
        }

    }

    // Find all topics that match the specified subscription
    template<typename Output>
    void find_match(MQTT_NS::string_view subscription, Output callback) const {
        std::deque<map_type_const_iterator> entries;
        entries.push_back(root);

        std::deque<map_type_const_iterator> new_entries;
        topic_filter_tokenizer(
            subscription,
            [this, &entries, &new_entries, &callback](MQTT_NS::string_view t) {
                new_entries.resize(0);

                for (auto const& entry : entries) {
                    node_id_t parent = entry->second.id;

                    if (t == MQTT_NS::string_view("+")) {
                        for (map_type_const_iterator i = map.lower_bound(path_entry_key(parent, MQTT_NS::string_view("")));
                             i != map.end();
                             ++i) {
                            if (i->first.first == parent &&
                                (parent != root_node_id || (i->first.second.empty() ? true : i->first.second[0] != '$'))
                            ) {
                                new_entries.push_back(i);
                            }
                            else {
                                break;
                            }
                        }
                    }
                    else if (t == MQTT_NS::string_view("#")) {
                        match_hash_entries(parent, callback, parent == root_node_id);
                        return false;
                    }
                    else {
                        map_type_const_iterator i = map.find(path_entry_key(parent, t));
                        if (i != map.end()) {
                            new_entries.push_back(i);
                        }
                    }
                }

                std::swap(new_entries, entries);
                return !entries.empty();
            }
        );

        for (auto const& entry : entries) {
            if (entry->second.value) {
                callback(*entry->second.value);
            }
        }
    }

    // Remove a value at the specified subscription path
    size_t erase_topic(MQTT_NS::string_view topic) {
        auto path = find_topic(topic);

        // Reset the value if there is actually something stored
        if (!path.empty() && path.back()->second.value) {
            path.back()->second.value = MQTT_NS::nullopt;

            for (auto entry : boost::adaptors::reverse(path)) {
                --entry->second.count;
                if (entry->second.count == 0) {
                    map.erase(entry->first);
                }
            }

            return 1;
        }

        return 0;
    }

    // Increase the number of topics for this path
    void increate_topics(std::vector<map_type_iterator> const &path) {
        for (auto i : path) {
            ++(i->second.count);
        }
    }

public:
    retained_topic_map()
    {
        // Create the root node
        root =
            map.emplace(
                path_entry_key(
                    std::numeric_limits<node_id_t>::max(),
                    MQTT_NS::allocate_buffer("")
                ),
                path_entry(root_node_id)
            ).first;
        ++next_node_id;
    }

    // Insert a value at the specified subscription path

    template <typename V>
    size_t insert_or_update(MQTT_NS::string_view topic, V&& value) {
        auto path = this->find_topic(topic);
        if (path.empty()) {
            this->create_topic(topic)->second.value.emplace(std::forward<V>(value));
            ++map_size;
            return 1;
        }

        if (!path.back()->second.value) {
            this->increate_topics(path);
            path.back()->second.value.emplace(std::forward<V>(value));
            ++map_size;
            return 1;
        }

        path.back()->second.value.emplace(std::forward<V>(value));
        return 0;
    }

    // Find all stored topics that math the specified subscription
    void find(MQTT_NS::string_view subscription, std::function< void (Value const&) > const& callback) const {
        find_match(subscription, callback);
    }

    // Remove a stored value at the specified topic
    std::size_t erase(MQTT_NS::string_view topic) {
        auto result = erase_topic(topic);
        map_size -= result;
        return result;
    }

    // Get the number of entries stored in the map
    std::size_t size() const { return map_size; }

    // Get the number of entries in the map (for debugging purpose only)
    std::size_t internal_size() const { return map.size(); }

    // Dump debug information
    template<typename Output>
    void dump(Output &out) {
        for (auto const& i : map) {
            out << i.first.first << " " << i.first.second << " " << (i.second.value ? "init" : "-") << " " << i.second.count << std::endl;
        }
    }

};

#endif // MQTT_RETAINED_TOPIC_MAP_HPP
