/*
 * This file is part of mcproxy.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * written by Sebastian Woelke, in cooperation with:
 * INET group, Hamburg University of Applied Sciences,
 * Website: http://mcproxy.realmv6.org/
 */

#include "include/hamcast_logging.h"
#include "include/parser/configuration.hpp"
#include "include/parser/parser.hpp"

#include <algorithm>
#include <fstream>

configuration::configuration(const std::string& path, bool reset_reverse_path_filter, bool in_debug_testing_mode)
    : m_in_debug_testing_mode(in_debug_testing_mode)
    , m_reset_reverse_path_filter(reset_reverse_path_filter)
    , m_gmp(IGMPv3) //default setting
    , m_global_table_set(std::make_shared<global_table_set>())
{
    HC_LOG_TRACE("");
    m_cmds = separate_commands(delete_comments(load_file(path)));
    run_parser();

    if (!m_in_debug_testing_mode) {
        initalize_interfaces();
    }
}

// trim from start
inline std::string& ltrim(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}

// trim from end
inline std::string& rtrim(std::string& s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

// trim from both ends
inline std::string& trim(std::string& s)
{
    return ltrim(rtrim(s));
}

inline unsigned int count_chars(const std::string& s, const char comp)
{
    unsigned int count = 0;
    std::for_each(s.begin(), s.end(), [&count, &comp](const char c) {
        if (c == comp) {
            ++count;
        }
    });
    return count;
}

std::string configuration::load_file(const std::string& path)
{
    HC_LOG_TRACE("");
    std::ifstream file;
    std::ostringstream result;
    std::string line;
    file.open (path, std::ifstream::in);
    if (!file) {
        HC_LOG_ERROR("failed to open config file: " << path);
        throw "failed to open config file";
    }

    while (std::getline(file, line)) {
        result << line << std::endl;
    }

    return result.str();
}

std::string configuration::delete_comments(std::string&& script_file)
{
    HC_LOG_TRACE("");

    const char comment_char = '#';
    const char end_of_comment = '\n';
    std::string::size_type cc = script_file.find(comment_char);
    std::string::size_type eoc;

    while (cc != std::string::npos) {
        eoc = script_file.find(end_of_comment, cc + 1);
        if (eoc == std::string::npos) {
            return script_file.substr(0, cc);
        } else {
            script_file.erase(cc, eoc - cc);
        }

        cc = script_file.find(comment_char, cc);
    }
    return script_file;
}

std::vector<std::pair<unsigned int, std::string>> configuration::separate_commands(std::string&& script_file)
{
    HC_LOG_TRACE("");
    std::stringstream ss(script_file);

    std::vector<std::pair<unsigned int, std::string>> result;
    const char cmd_separator = ';';
    std::string item;

    unsigned int current_line = 1;

    while (std::getline(ss, item, cmd_separator)) {
        unsigned int line_count_before = count_chars(item, '\n');
        ltrim(item);
        unsigned int line_count_after = count_chars(item, '\n');
        rtrim(item);
        if (!item.empty()) {
            result.push_back(std::pair<unsigned int, std::string>((current_line + (line_count_before - line_count_after)), item));
        }
        current_line += line_count_before;
    }

    return result;
}

void configuration::run_parser()
{
    HC_LOG_TRACE("");

    for (auto e : m_cmds) {
        parser p(e.first, e.second);
        switch (p.get_parser_type()) {
        case PT_PROTOCOL: {
            m_gmp = p.parse_group_mem_proto();
            break;
        }
        case PT_INSTANCE_DEFINITION: {
            p.parse_instance_definition(m_inst_def_set);
            break;
        }
        case PT_TABLE: {
            auto t = p.parse_table(m_global_table_set, m_gmp);
            std::string table_name = t->get_name();
            if (!m_global_table_set->insert(std::move(t))) {
                HC_LOG_ERROR("faild to parse configfile table " << table_name << " already exists");
                throw "failed to parse configfile";
            }
            break;
        }
        case PT_INTERFACE_RULE_BINDING: {
            p.parse_interface_rule_binding(m_global_table_set, m_gmp, m_inst_def_set);
            break;
        }
        default:
            HC_LOG_ERROR("unkown parser type");
            throw "unkown parser type";
        }
    }
}

void configuration::initalize_interfaces()
{
    HC_LOG_TRACE("");

    unsigned int if_index;

    for (auto & inst : m_inst_def_set) {
        auto result = std::make_shared<interfaces>(get_addr_family(m_gmp), m_reset_reverse_path_filter);
        auto add = [&](const std::shared_ptr<interface>& interf) {
            if_index = interfaces::get_if_index(interf->get_if_name());
            if (if_index == 0) {
                HC_LOG_ERROR("interface " << interf->get_if_name() << " not found");
                throw "unknown interface";
            }

            if (!result->add_interface(if_index)) {
                throw "failed to add interface";
            }
        };

        for (auto & downstream : inst->get_downstreams()) {
            add(downstream);
        }

        for (auto & upstream : inst->get_upstreams()) {
            add(upstream);
        }

        if (!m_interfaces_map.insert(std::pair<std::string, std::shared_ptr<interfaces>>(inst->get_instance_name(), result)).second) {
            HC_LOG_ERROR("proxy instance " << inst->get_instance_name() << " already exists");
            throw "failed to add instance";
        }
    }
}

#ifdef DEBUG_MODE
void configuration::test_configuration()
{
    using namespace std;
    cout << "start programm" << endl;

    configuration conf("../references/parser/test_configs/test3.conf", false, true);
    cout << conf.to_string() << endl;

    auto& inst_def_set = conf.get_inst_def_set();
    for (auto & e : inst_def_set) {
        test_myProxy(e);
    }


    //---------------------------------------------------------
    //delete comments
    //cout << "1<" << s.delete_comments("#1234\n1234") << ">" << endl;
    //cout << "2<" << s.delete_comments("1234\n#1234") << ">" << endl;
    //cout << "3<" << s.delete_comments("#\n1234\n#1234") << ">" << endl;
    //cout << "4<" << s.delete_comments("1234#1234\n") << ">" << endl;
    //cout << "5<" << s.delete_comments("1234") << ">" << endl;
    //cout << "6<" << s.delete_comments("") << ">" << endl;
    //cout << "7<" << s.delete_comments("##\n1234") << ">" << endl;
    //cout << "8<" << s.delete_comments("\n1234#") << ">" << endl;
    //cout << "9<" << s.delete_comments("#12#34\n#56#78#910\n\n\n1234#") << ">" << endl;
    //cout << "10<" << s.delete_comments("#12#34\n#56#78#910\n1\n2\n3\n4#") << ">" << endl;
    //cout << "11<" << s.delete_comments("1234#\n") << ">" << endl;

    //auto tmp  = s.separate_commands(";asd; ad; xx;;;");
    //std::for_each(tmp.begin(), tmp.end(), [](std::string & n) {
    //std::cout << n << std::endl;
    //});

    //std::for_each(s.m_cmds.begin(), s.m_cmds.end(), [&](std::pair<unsigned int, std::string>& n) {
    //std::cout << n.first << ":=" << n.second << std::endl;
    //});
    //

    cout << "end of programm" << endl;
}

void configuration::test_myProxy(const std::shared_ptr<instance_definition>& id)
{
    using namespace std;
    cout << "##-- Test: " << id->get_instance_name() << " --##" << endl;

    const interface& interf = *(id->get_downstreams().begin()->get());
    cout << " - if_name: " << interf.get_if_name() << endl;

    cout << " - in filter type: ";
    if (interf.get_filter_type(ID_OUT) == FT_BLACKLIST) {
        cout << "BLACKLIST" << endl;
    } else if (interf.get_filter_type(ID_IN) == FT_WHITELIST) {
        cout << "WHITELIST" << endl;
    } else {
        cout << "failed to get filter type!!!" << endl;
        return;
    }

    cout << " - source list of(if: \"\", gaddr: 99): " << interf.get_saddr_set(ID_IN, "", addr_storage("99.99.99.99")) << endl;
    cout << " - source list of(if: \"xx\", gaddr: 99): " << interf.get_saddr_set(ID_IN, "xx", addr_storage("99.99.99.99")) << endl;
}

void configuration::test_source_allowed()
{
    using namespace std;
    cout << "start programm" << endl;

    configuration conf("../references/parser/test_configs/test3.conf", false, true);
    cout << conf.to_string() << endl;


    for (auto & e : conf.get_inst_def_set()) {
        const interface& interf = *(e->get_downstreams().begin()->get());
        auto& inst_name = e->get_instance_name();
        auto test_fun = [&inst_name](bool result, bool expected) {
            cout << inst_name << " ";
            if (result){
                cout << "allowed ==> ";
            }else{
                cout << "not allowed ==> ";
            }

            if (result == expected) {
                cout << "OK!" << endl;
            } else {
                cout << "FAILED!" << endl;
            }
        };

        if (inst_name.compare("myProxy01") == 0) {
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy02") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy03") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy04") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy05") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy06") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy07") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy08") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy09") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy10") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy11") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy12") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy13") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, true);
        } else if (inst_name.compare("myProxy14") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        } else if (inst_name.compare("myProxy15") == 0){
            bool result = interf.is_source_allowed(ID_IN, "a", addr_storage("239.99.99.99"), addr_storage("2.2.2.3"));
            test_fun(result, false);
        }else{
            cout << "ERROR!! Unkown instance name!!" << endl; 
        }

    }
    cout << "end of programm" << endl;
}

#endif /* DEBUG_MODE */

const std::shared_ptr<const interfaces> configuration::get_interfaces_for_pinstance(const std::string& instance_name) const
{
    HC_LOG_TRACE("");
    auto it = m_interfaces_map.find(instance_name);
    if (it == m_interfaces_map.end()) {
        return nullptr;
    } else {
        return it->second;
    }
}

group_mem_protocol configuration::get_group_mem_protocol() const
{
    HC_LOG_TRACE("");
    return m_gmp;
}

const inst_def_set& configuration::get_inst_def_set() const
{
    HC_LOG_TRACE("");
    return m_inst_def_set;
}

std::string configuration::to_string() const
{
    HC_LOG_TRACE("");
    using namespace std;
    ostringstream s;
    s << "##-- proxy configuration --##" << endl;
    s << "protocol " << get_group_mem_protocol_name(m_gmp) << endl;
    s << m_global_table_set->to_string() << endl;
    s << m_inst_def_set.to_string() << endl;
    s << endl;
    for (auto & e : m_interfaces_map) {
        s << e.second->to_string() << endl;
    }
    return s.str();
}
