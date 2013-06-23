/*
    This file is part of Corrade.

    Copyright © 2007, 2008, 2009, 2010, 2011, 2012, 2013
              Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "Configuration.h"

#include <fstream>

#include "Utility/Assert.h"
#include "Utility/Debug.h"
#include "Utility/String.h"

namespace Corrade { namespace Utility {

Configuration::Configuration(Configuration::Flags flags): ConfigurationGroup(this), flags(static_cast<InternalFlag>(std::uint32_t(flags))|InternalFlag::IsValid) {}

Configuration::Configuration(const std::string& filename, Flags flags): ConfigurationGroup(this), _filename(filename), flags(static_cast<InternalFlag>(std::uint32_t(flags))) {
    /* Open file with requested flags */
    std::ifstream::openmode openmode = std::ifstream::in;
    if(this->flags & InternalFlag::Truncate) openmode |= std::ifstream::trunc;
    std::ifstream file(filename.c_str(), openmode);

    /* File doesn't exist yet */
    if(!file.is_open()) {
        /** @todo check better */

        /* It is error for readonly configurations */
        /** @todo Similar check for istream constructor (?) */
        if(this->flags & InternalFlag::ReadOnly) return;

        this->flags |= InternalFlag::IsValid;
        return;
    }

    parse(file);

    /* Close file */
    file.close();
}

Configuration::Configuration(std::istream& file, Flags flags): ConfigurationGroup(this), flags(static_cast<InternalFlag>(std::uint32_t(flags))) {
    parse(file);

    /* Set readonly flag, because the configuration cannot be saved */
    this->flags |= InternalFlag::ReadOnly;
}

Configuration::~Configuration() { if(flags & InternalFlag::Changed) save(); }

std::string Configuration::filename() const { return _filename; }

void Configuration::setFilename(std::string filename) {
    _filename = std::move(filename);
}

void Configuration::parse(std::istream& file) {
    try {
        if(!file.good())
            throw std::string("Cannot open configuration file.");

        /* It looks like BOM */
        if(file.peek() == String::Bom[0]) {
            char* bom = new char[4];
            file.get(bom, 4);

            /* This is not a BOM, rewind back */
            if(bom != String::Bom) file.seekg(0);

            /* Or set flag */
            else flags |= InternalFlag::HasBom;

            delete[] bom;
        }

        /* Parse file */
        parse(file, this, {});

        /* Everything went fine */
        flags |= InternalFlag::IsValid;

    } catch(std::string e) { Error() << e; }
}

std::string Configuration::parse(std::istream& file, ConfigurationGroup* group, const std::string& fullPath) {
    std::string buffer;

    /* Parse file */
    bool multiLineValue = false;
    while(file.good()) {
        std::getline(file, buffer);

        /* Windows EOL */
        if(buffer.back() == '\r')
            flags |= InternalFlag::WindowsEol;

        /* Multi-line value */
        if(multiLineValue) {
            /* End of multi-line value */
            if(String::trim(buffer) == "\"\"\"") {
                /* Remove trailing newline, if present */
                if(!group->items.back().value.empty()) {
                    CORRADE_INTERNAL_ASSERT(group->items.back().value.back() == '\n');
                    group->items.back().value.resize(group->items.back().value.size()-1);
                }

                multiLineValue = false;
                continue;
            }

            /* Remove Windows EOL, if present */
            if(buffer.back() == '\r') buffer.resize(buffer.size()-1);

            /* Append it (with newline) to current value */
            group->items.back().value += buffer;
            group->items.back().value += '\n';
            continue;
        }

        /* Trim buffer */
        buffer = String::trim(buffer);

        /* Group header */
        if(buffer[0] == '[') {

            /* Check ending bracket */
            if(buffer[buffer.size()-1] != ']')
                throw std::string("Missing closing bracket for group header!");

            std::string nextGroup = String::trim(buffer.substr(1, buffer.size()-2));

            if(nextGroup.empty())
                throw std::string("Empty group name!");

            /* Next group is subgroup of current group, recursive call */
            while(!nextGroup.empty() && (fullPath.empty() || nextGroup.substr(0, fullPath.size()) == fullPath)) {
                ConfigurationGroup::Group g;
                g.name = nextGroup.substr(fullPath.size());
                g.group = new ConfigurationGroup(configuration);
                nextGroup = parse(file, g.group, nextGroup+'/');

                /* If unique groups are set, check whether current group is unique */
                bool save = true;
                if(flags & InternalFlag::UniqueGroups) {
                    /** @todo Do this in logarithmic time */
                    for(auto it = group->_groups.cbegin(); it != group->_groups.cend(); ++it)
                        if(it->name == g.name) {
                            save = false;
                            break;
                        }
                }
                if(save) group->_groups.push_back(g);
            }

            return nextGroup;

        /* Empty line */
        } else if(buffer.empty()) {
            if(flags & (InternalFlag::SkipComments|InternalFlag::ReadOnly)) continue;

            group->items.push_back(ConfigurationGroup::Item());

        /* Comment */
        } else if(buffer[0] == '#' || buffer[0] == ';') {
            if(flags & (InternalFlag::SkipComments|InternalFlag::ReadOnly)) continue;

            ConfigurationGroup::Item item;
            item.value = buffer;
            group->items.push_back(item);

        /* Key/value pair */
        } else {
            const std::size_t splitter = buffer.find_first_of('=');
            if(splitter == std::string::npos)
                throw std::string("Key/value pair without '=' character!");

            ConfigurationGroup::Item item;
            item.key = String::trim(buffer.substr(0, splitter));
            item.value = String::trim(buffer.substr(splitter+1));

            /* Start of multi-line value */
            if(item.value == "\"\"\"") {
                item.value = "";
                multiLineValue = true;

            /* Remove quotes, if present */
            /** @todo Check `"` characters better */
            } else if(!item.value.empty() && item.value[0] == '"') {
                if(item.value.size() < 2 || item.value[item.value.size()-1] != '"')
                    throw std::string("Missing closing quotes in value!");

                item.value = item.value.substr(1, item.value.size()-2);
            }

            /* If unique keys are set, check whether current key is unique */
            if(flags & InternalFlag::UniqueKeys) {
                /** @todo fixme: get rid of this flag altogether */
                CORRADE_INTERNAL_ASSERT(!multiLineValue);
                bool contains = false;
                for(auto it = group->items.cbegin(); it != group->items.cend(); ++it)
                    if(it->key == item.key) {
                        contains = true;
                        break;
                    }
                if(contains) continue;
            }

            group->items.push_back(item);
        }
    }

    /* Remove last empty line, if present (will be written automatically) */
    if(!group->items.empty() && group->items.back().key.empty() && group->items.back().value.empty())
        group->items.pop_back();

    /* This was the last group */
    return {};
}

bool Configuration::save(const std::string& filename) {
    std::ofstream out(filename.c_str(), std::ofstream::out|std::ofstream::trunc|std::ofstream::binary);
    if(!out.good()) {
        /** @todo Error to stderr */
        return false;
    }

    return save(out);
}

bool Configuration::save(std::ostream& out) {
    /* BOM, if user explicitly wants that crap */
    if((flags & InternalFlag::PreserveBom) && (flags & InternalFlag::HasBom))
        out.write(String::Bom.c_str(), 3);

    /* EOL character */
    std::string eol;
    if(flags & (InternalFlag::ForceWindowsEol|InternalFlag::WindowsEol) && !(flags & InternalFlag::ForceUnixEol)) eol = "\r\n";
    else eol = "\n";

    std::string buffer;

    /** @todo Checking file.good() after every operation */
    /** @todo Backup file */

    /* Recursively save all groups */
    save(out, eol, this, {});
    return true;
}

bool Configuration::save() {
    /* File is readonly or invalid, don't save anything */
    if(flags & InternalFlag::ReadOnly || !(flags & InternalFlag::IsValid)) return false;

    return save(_filename);
}

void Configuration::save(std::ostream& file, const std::string& eol, ConfigurationGroup* group, const std::string& fullPath) const {
    std::string buffer;

    /* Foreach all items in the group */
    for(auto it = group->items.cbegin(); it != group->items.cend(); ++it) {
        /* Key/value pair */
        if(!it->key.empty()) {
            /* Multi-line value */
            if(it->value.find_first_of('\n') != std::string::npos) {
                /* Replace \n with `eol` */
                /** @todo fixme: ugly and slow */
                std::string value = it->value;
                std::size_t pos = 0;
                while((pos = value.find_first_of('\n', pos)) != std::string::npos) {
                    value.replace(pos, 1, eol);
                    pos += eol.size();
                }

                buffer = it->key + "=\"\"\"" + eol + value + eol + "\"\"\"" + eol;

            /* Value with spaces */
            } else if(it->value.find_first_of(String::Whitespace) != std::string::npos)
                buffer = it->key + "=\"" + it->value + '"' + eol;

            /* Value without spaces */
            else buffer = it->key + '=' + it->value + eol;
        }

        /* Comment / empty line */
        else buffer = it->value + eol;

        file.write(buffer.c_str(), buffer.size());
    }

    /* Recursively process all subgroups */
    for(auto git = group->_groups.cbegin(); git != group->_groups.cend(); ++git) {
        /* Subgroup name */
        std::string name = git->name;
        if(!fullPath.empty()) name = fullPath + '/' + name;

        buffer = '[' + name + ']' + eol;
        file.write(buffer.c_str(), buffer.size());

        save(file, eol, git->group, name);
    }
}

}}
