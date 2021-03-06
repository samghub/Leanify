#include "xml.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <string>

#include "../leanify.h"
#include "../utils.h"
#include "base64.h"

using std::cout;
using std::endl;
using std::string;


Xml::Xml(void *p, size_t s /*= 0*/) : Format(p, s)
{
    pugi::xml_parse_result result = doc_.load_buffer(fp_, size_, pugi::parse_default | pugi::parse_declaration | pugi::parse_doctype | pugi::parse_ws_pcdata_single);
    is_valid_ = result;
    encoding_ = result.encoding;
}


namespace
{

// shrink spaces and newlines, also removes preceding and trailing spaces
string ShrinkSpace(const char* value)
{
    string new_value;
    while (*value)
    {
        if (*value == ' ' || *value == '\n' || *value == '\t')
        {
            do
            {
                value++;
            } while (*value == ' ' || *value == '\n' || *value == '\t');
            if (*value == 0)
            {
                break;
            }
            new_value += ' ';
        }
        new_value += *value++;
    }
    if (!new_value.empty() && new_value[0] == ' ')
    {
        new_value.erase(0, 1);
    }
    return new_value;
}

void TraverseElements(pugi::xml_node node, std::function<void(pugi::xml_node)> callback)
{
    // cannot use ranged loop here because we need to store the next_sibling before recursion so that if child was removed the loop won't be terminated
    for (pugi::xml_node child = node.first_child(), next; child; child = next)
    {
        next = child.next_sibling();
        TraverseElements(child, callback);
    }

    callback(node);
}

// Remove single PCData only contains whitespace if xml:space="preserve" is not set.
void RemovePCDataSingle(pugi::xml_node node, bool xml_space_preserve)
{
    xml_space_preserve |= strcmp(node.attribute("xml:space").value(), "preserve") == 0;
    if (xml_space_preserve)
    {
        return;
    }
    pugi::xml_node pcdata = node.first_child();
    if (pcdata.first_child() == nullptr &&
        pcdata.type() == pugi::node_pcdata &&
        pcdata.next_sibling() == nullptr)
    {
        if (ShrinkSpace(pcdata.value()).empty())
        {
            node.remove_child(pcdata);
        }
        return;
    }
    for (pugi::xml_node child : node.children())
    {
        RemovePCDataSingle(child, xml_space_preserve);
    }
}

struct xml_memory_writer : pugi::xml_writer
{
    uint8_t *p_write;

    void write(const void* data, size_t size) override
    {
        memcpy(p_write, data, size);
        p_write += size;
    }
};

} // namespace


size_t Xml::Leanify(size_t size_leanified /*= 0*/)
{
    RemovePCDataSingle(doc_, false);

    // if the XML is fb2 file
    if (doc_.child("FictionBook"))
    {
        if (is_verbose)
        {
            cout << "FB2 detected." << endl;
        }
        if (depth < max_depth)
        {
            depth++;

            pugi::xml_node root = doc_.child("FictionBook");

            // iterate through all binary element
            for (pugi::xml_node binary = root.child("binary"), next; binary; binary = next)
            {
                next = binary.next_sibling("binary");
                pugi::xml_attribute id = binary.attribute("id");
                if (id == nullptr || id.value() == nullptr || id.value()[0] == 0)
                {
                    root.remove_child(binary);
                    continue;
                }

                PrintFileName(id.value());

                const char *base64_data = binary.child_value();
                if (base64_data == nullptr)
                {
                    cout << "No data found." << endl;
                    continue;
                }
                size_t base64_len = strlen(base64_data);
                // copy to a new location because base64_data is const
                char *new_base64_data = new char[base64_len + 1];
                memcpy(new_base64_data, base64_data, base64_len);

                Base64 b64(new_base64_data, base64_len);
                size_t new_base64_len = b64.Leanify();

                if (new_base64_len < base64_len)
                {
                    new_base64_data[new_base64_len] = 0;
                    binary.text() = new_base64_data;
                }

                delete[] new_base64_data;
            }
            depth--;
        }
    }
    else if (doc_.child("svg"))
    {
        if (is_verbose)
        {
            cout << "SVG detected." << endl;
        }

        // remove XML declaration and doctype
        for (pugi::xml_node child = doc_.first_child(), next; child; child = next)
        {
            next = child.next_sibling();
            if (child.type() == pugi::node_declaration || child.type() == pugi::node_doctype)
            {
                doc_.remove_child(child);
            }
        }

        TraverseElements(doc_.child("svg"), [](pugi::xml_node node)
        {
            for (pugi::xml_attribute attr = node.first_attribute(), next; attr; attr = next)
            {
                next = attr.next_attribute();
                const char* value = attr.value();
                // remove empty attribute
                if (value == nullptr || *value == 0)
                {
                    node.remove_attribute(attr);
                    continue;
                }

                attr = ShrinkSpace(value).c_str();
            }

            // remove empty text element and container element
            const char* name = node.name();
            if (node.first_child() == nullptr)
            {
                if (strcmp(name, "text") == 0 ||
                    strcmp(name, "tspan") == 0 ||
                    strcmp(name, "a") == 0 ||
                    strcmp(name, "defs") == 0 ||
                    strcmp(name, "g") == 0 ||
                    strcmp(name, "marker") == 0 ||
                    strcmp(name, "mask") == 0 ||
                    strcmp(name, "missing-glyph") == 0 ||
                    strcmp(name, "pattern") == 0 ||
                    strcmp(name, "switch") == 0 ||
                    strcmp(name, "symbol") == 0)
                {
                    node.parent().remove_child(node);
                    return;
                }
            }

            if (strcmp(name, "tref") == 0 && node.attribute("xlink:href") == nullptr)
            {
                node.parent().remove_child(node);
                return;
            }

            if (strcmp(name, "metadata") == 0)
            {
                node.parent().remove_child(node);
                return;
            }
        });
    }

    // print leanified XML to memory
    xml_memory_writer writer;
    fp_ -= size_leanified;
    writer.p_write = fp_;
    doc_.save(writer, nullptr, pugi::format_raw | pugi::format_no_declaration, encoding_);
    size_ = writer.p_write - fp_;
    return size_;
}
