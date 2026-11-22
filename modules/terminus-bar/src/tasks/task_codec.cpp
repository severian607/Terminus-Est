#include "tasks/task_codec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <libical/ical.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <memory>
#include <sodium.h>

namespace tasks {
  namespace {
    using Calendar = std::unique_ptr<icalcomponent, decltype(&icalcomponent_free)>;
    using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    std::string part(CURLU* url, CURLUPart kind, unsigned flags = 0) {
      char* raw = nullptr;
      if (curl_url_get(url, kind, &raw, flags) != CURLUE_OK)
        return {};
      std::string value(raw);
      curl_free(raw);
      return value;
    }
    bool cleanUrl(CURLU* url) {
      const auto scheme = part(url, CURLUPART_SCHEME);
      return (scheme == "http" || scheme == "https")
          && !part(url, CURLUPART_HOST).empty()
          && part(url, CURLUPART_USER).empty()
          && part(url, CURLUPART_PASSWORD).empty()
          && part(url, CURLUPART_QUERY).empty()
          && part(url, CURLUPART_FRAGMENT).empty();
    }
    bool cleanPath(const std::string& path) {
      if (path.empty() || path.front() != '/')
        return false;
      std::size_t start = 1;
      for (std::size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = path[i];
        if (c < 32 || c == 127 || c == '\\')
          return false;
        if (i >= start && (c == '/' || i + 1 == path.size())) {
          const auto segment = path.substr(start, i - start + (c != '/'));
          if (segment == "." || segment == "..")
            return false;
          start = i + 1;
        }
      }
      return true;
    }
    Calendar parse(const std::string& data) {
      Calendar root(icalparser_parse_string(data.c_str()), icalcomponent_free);
      if (!root
          || icalcomponent_isa(root.get()) != ICAL_VCALENDAR_COMPONENT
          || icalcomponent_count_errors(root.get()) != 0)
        return {nullptr, icalcomponent_free};
      return root;
    }
    icalcomponent* todo(icalcomponent* root) {
      icalcomponent* first = nullptr;
      for (auto* c = icalcomponent_get_first_component(root, ICAL_VTODO_COMPONENT); c;
           c = icalcomponent_get_next_component(root, ICAL_VTODO_COMPONENT)) {
        if (!first)
          first = c;
        if (!icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY))
          return c;
      }
      return first;
    }
    std::string stringValue(icalcomponent* c, icalproperty_kind kind) {
      auto* p = icalcomponent_get_first_property(c, kind);
      if (!p)
        return {};
      const char* v = kind == ICAL_SUMMARY_PROPERTY ? icalproperty_get_summary(p) : icalproperty_get_description(p);
      return v ? v : "";
    }
    TaskItem item(icalcomponent* c) {
      TaskItem result;
      result.title = stringValue(c, ICAL_SUMMARY_PROPERTY);
      result.description = stringValue(c, ICAL_DESCRIPTION_PROPERTY);
      auto* due = icalcomponent_get_first_property(c, ICAL_DUE_PROPERTY);
      if (due) {
        auto date = icalcomponent_get_due(c);
        if (!date.is_date && !icaltime_is_null_time(date)) {
          const icaltimezone* zone = icaltime_get_timezone(date);
          if (zone == nullptr) {
            if (auto* parameter = icalproperty_get_first_parameter(due, ICAL_TZID_PARAMETER)) {
              const char* tzid = icalparameter_get_tzid(parameter);
              zone = icalcomponent_get_timezone(icalcomponent_get_parent(c), tzid);
              if (zone == nullptr)
                zone = icaltimezone_get_builtin_timezone(tzid);
            }
          }
          if (zone != nullptr) {
            const auto instant = static_cast<std::time_t>(icaltime_as_timet_with_zone(date, zone));
            std::tm local{};
            if (localtime_r(&instant, &local) != nullptr) {
              date.year = local.tm_year + 1900;
              date.month = local.tm_mon + 1;
              date.day = local.tm_mday;
            }
          }
        }
        if (!icaltime_is_null_time(date)) {
          char buffer[32];
          std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", date.year, date.month, date.day);
          result.dueDate = buffer;
        }
      }
      auto* status = icalcomponent_get_first_property(c, ICAL_STATUS_PROPERTY);
      result.completed = (status && icalproperty_get_status(status) == ICAL_STATUS_COMPLETED)
          || icalcomponent_get_first_property(c, ICAL_COMPLETED_PROPERTY);
      return result;
    }
    void removeProperties(icalcomponent* c, icalproperty_kind kind) {
      while (auto* p = icalcomponent_get_first_property(c, kind)) {
        icalcomponent_remove_property(c, p);
        icalproperty_free(p);
      }
    }
    void replace(icalcomponent* c, icalproperty_kind kind, icalproperty* replacement) {
      removeProperties(c, kind);
      if (replacement)
        icalcomponent_add_property(c, replacement);
    }
    void updateFields(icalcomponent* c, const TaskItem& old, const TaskDraft* draft, std::optional<bool> completed) {
      if (draft) {
        if (draft->title != old.title)
          replace(c, ICAL_SUMMARY_PROPERTY, icalproperty_new_summary(draft->title.c_str()));
        if (draft->description != old.description) {
          replace(
              c, ICAL_DESCRIPTION_PROPERTY,
              draft->description.empty() ? nullptr : icalproperty_new_description(draft->description.c_str())
          );
        }
        if (draft->dueDate != old.dueDate) {
          if (draft->dueDate.empty())
            replace(c, ICAL_DUE_PROPERTY, nullptr);
          else {
            auto compact = draft->dueDate;
            std::erase(compact, '-');
            replace(c, ICAL_DUE_PROPERTY, icalproperty_new_due(icaltime_from_string(compact.c_str())));
            // DURATION and DUE are mutually exclusive in a VTODO.
            removeProperties(c, ICAL_DURATION_PROPERTY);
          }
        }
      }
      const auto now = icaltime_current_time_with_zone(icaltimezone_get_utc_timezone());
      if (completed) {
        replace(
            c, ICAL_STATUS_PROPERTY,
            icalproperty_new_status(*completed ? ICAL_STATUS_COMPLETED : ICAL_STATUS_NEEDSACTION)
        );
        replace(c, ICAL_PERCENTCOMPLETE_PROPERTY, icalproperty_new_percentcomplete(*completed ? 100 : 0));
        replace(c, ICAL_COMPLETED_PROPERTY, *completed ? icalproperty_new_completed(now) : nullptr);
      }
      replace(c, ICAL_DTSTAMP_PROPERTY, icalproperty_new_dtstamp(now));
      replace(c, ICAL_LASTMODIFIED_PROPERTY, icalproperty_new_lastmodified(now));
    }
    std::string serialize(icalcomponent* c) {
      char* raw = icalcomponent_as_ical_string_r(c);
      if (!raw)
        return {};
      std::string result(raw);
      icalmemory_free_buffer(raw);
      return result;
    }
    std::string canonical(icalcomponent* c) {
      std::vector<std::string> values;
      for (auto* p = icalcomponent_get_first_property(c, ICAL_ANY_PROPERTY); p;
           p = icalcomponent_get_next_property(c, ICAL_ANY_PROPERTY)) {
        char* raw = icalproperty_as_ical_string_r(p);
        if (raw) {
          values.emplace_back(raw);
          icalmemory_free_buffer(raw);
        }
      }
      for (auto* child = icalcomponent_get_first_component(c, ICAL_ANY_COMPONENT); child;
           child = icalcomponent_get_next_component(c, ICAL_ANY_COMPONENT))
        values.push_back(canonical(child));
      std::sort(values.begin(), values.end());
      std::string result = icalcomponent_kind_to_string(icalcomponent_isa(c));
      for (const auto& value : values)
        result += std::to_string(value.size()) + ":" + value;
      return result;
    }
    bool named(xmlNode* node, const char* name, const char* ns) {
      return node
          && node->type == XML_ELEMENT_NODE
          && node->ns
          && node->ns->href
          && xmlStrEqual(node->name, BAD_CAST name)
          && xmlStrEqual(node->ns->href, BAD_CAST ns);
    }
    xmlNode* child(xmlNode* parent, const char* name, const char* ns) {
      for (auto* c = parent->children; c; c = c->next)
        if (named(c, name, ns))
          return c;
      return nullptr;
    }
    std::string text(xmlNode* node) {
      if (!node)
        return {};
      xmlChar* raw = xmlNodeGetContent(node);
      std::string value = raw ? reinterpret_cast<char*>(raw) : "";
      xmlFree(raw);
      return value;
    }
    bool successful(const std::string& status) {
      auto pos = status.find(' ');
      return status.starts_with("HTTP/") && pos != std::string::npos && status.substr(pos + 1, 3) == "200";
    }
  } // namespace

  std::optional<std::string> collectionUrl(std::string_view input) {
    if (input.empty() || input.find_first_of("\r\n\t ?#") != std::string_view::npos)
      return std::nullopt;
    Url url(curl_url(), curl_url_cleanup);
    if (!url
        || curl_url_set(url.get(), CURLUPART_URL, std::string(input).c_str(), CURLU_PATH_AS_IS) != CURLUE_OK
        || !cleanUrl(url.get())
        || !cleanPath(part(url.get(), CURLUPART_PATH, CURLU_URLDECODE)))
      return std::nullopt;
    auto path = part(url.get(), CURLUPART_PATH);
    if (!path.ends_with('/')) {
      path += '/';
      if (curl_url_set(url.get(), CURLUPART_PATH, path.c_str(), 0) != CURLUE_OK)
        return std::nullopt;
    }
    return part(url.get(), CURLUPART_URL);
  }
  std::optional<std::string> resourceUrl(const std::string& collection, std::string_view href) {
    if (href.empty() || href.find_first_of("\r\n\t ?#") != std::string_view::npos)
      return std::nullopt;
    Url base(curl_url(), curl_url_cleanup), url(curl_url(), curl_url_cleanup);
    if (!base
        || !url
        || curl_url_set(base.get(), CURLUPART_URL, collection.c_str(), 0) != CURLUE_OK
        || curl_url_set(url.get(), CURLUPART_URL, collection.c_str(), 0) != CURLUE_OK
        || curl_url_set(url.get(), CURLUPART_URL, std::string(href).c_str(), CURLU_PATH_AS_IS) != CURLUE_OK
        || !cleanUrl(url.get()))
      return std::nullopt;
    for (auto kind : {CURLUPART_SCHEME, CURLUPART_HOST, CURLUPART_PORT}) {
      if (part(url.get(), kind, CURLU_DEFAULT_PORT) != part(base.get(), kind, CURLU_DEFAULT_PORT))
        return std::nullopt;
    }
    const auto path = part(url.get(), CURLUPART_PATH, CURLU_URLDECODE);
    const auto prefix = part(base.get(), CURLUPART_PATH, CURLU_URLDECODE);
    if (!cleanPath(path)
        || !path.starts_with(prefix)
        || path.size() <= prefix.size()
        || path.find('/', prefix.size()) != std::string::npos)
      return std::nullopt;
    // Use a single canonical encoding so escaped/unescaped href spellings share identity.
    if (curl_url_set(url.get(), CURLUPART_PATH, path.c_str(), CURLU_URLENCODE) != CURLUE_OK)
      return std::nullopt;
    return part(url.get(), CURLUPART_URL);
  }
  bool strongEtag(const std::string& etag) {
    return etag.size() >= 2
        && etag.front() == '"'
        && etag.back() == '"'
        && etag.find_first_of("\r\n") == std::string::npos;
  }
  bool parseReport(const std::string& body, const std::string& collection, Resources& out) {
    if (body.empty() || body.size() > 32 * 1024 * 1024)
      return false;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(
            body.data(), static_cast<int>(body.size()), "tasks.xml", nullptr,
            XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING
        ),
        xmlFreeDoc
    );
    if (!doc || doc->intSubset || doc->extSubset)
      return false;
    auto* root = xmlDocGetRootElement(doc.get());
    if (!named(root, "multistatus", "DAV:"))
      return false;
    Resources parsed;
    for (auto* response = root->children; response; response = response->next) {
      if (!named(response, "response", "DAV:"))
        continue;
      auto href = resourceUrl(collection, text(child(response, "href", "DAV:")));
      if (!href)
        return false;
      Resource resource;
      for (auto* propstat = response->children; propstat; propstat = propstat->next) {
        if (!named(propstat, "propstat", "DAV:"))
          continue;
        if (!successful(text(child(propstat, "status", "DAV:"))))
          return false;
        auto* prop = child(propstat, "prop", "DAV:");
        if (!prop)
          return false;
        if (auto* etag = child(prop, "getetag", "DAV:"))
          resource.etag = text(etag);
        if (auto* data = child(prop, "calendar-data", "urn:ietf:params:xml:ns:caldav"))
          resource.data = text(data);
      }
      if (!strongEtag(resource.etag)
          || !parseTask(resource.data)
          || !parsed.emplace(*href, std::move(resource)).second)
        return false;
    }
    out = std::move(parsed);
    return true;
  }
  std::optional<TaskItem> parseTask(const std::string& data) {
    auto root = parse(data);
    auto* c = root ? todo(root.get()) : nullptr;
    if (!c || !icalcomponent_get_first_property(c, ICAL_UID_PROPERTY))
      return std::nullopt;
    return item(c);
  }
  bool validDraft(const TaskDraft& draft) {
    if (draft.title.empty()
        || draft.title.find_first_not_of(" \t\r\n") == std::string::npos
        || draft.title.size() > 8192
        || draft.description.size() > 1024 * 1024
        || draft.title.find('\0') != std::string::npos
        || draft.description.find('\0') != std::string::npos)
      return false;
    if (draft.dueDate.empty())
      return true;
    if (draft.dueDate.size() != 10 || draft.dueDate[4] != '-' || draft.dueDate[7] != '-')
      return false;
    for (std::size_t i = 0; i < 10; ++i)
      if (i != 4 && i != 7 && (draft.dueDate[i] < '0' || draft.dueDate[i] > '9'))
        return false;
    const int y = std::stoi(draft.dueDate.substr(0, 4));
    const auto m = static_cast<unsigned>(std::stoi(draft.dueDate.substr(5, 2)));
    const auto d = static_cast<unsigned>(std::stoi(draft.dueDate.substr(8, 2)));
    return y > 0 && std::chrono::year_month_day(std::chrono::year(y), std::chrono::month(m), std::chrono::day(d)).ok();
  }
  std::string newTask(const TaskDraft& draft, const std::string& uid) {
    Calendar root(icalcomponent_new_vcalendar(), icalcomponent_free);
    icalcomponent_add_property(root.get(), icalproperty_new_version("2.0"));
    icalcomponent_add_property(root.get(), icalproperty_new_prodid("-//TerminusBar//Tasks//EN"));
    auto* c = icalcomponent_new_vtodo();
    icalcomponent_add_component(root.get(), c);
    icalcomponent_add_property(c, icalproperty_new_uid(uid.c_str()));
    icalcomponent_add_property(
        c, icalproperty_new_created(icaltime_current_time_with_zone(icaltimezone_get_utc_timezone()))
    );
    updateFields(c, {}, &draft, false);
    return serialize(root.get());
  }
  std::string restoreTask(const std::string& source, const std::string& uid) {
    auto root = parse(source);
    auto* master = root ? todo(root.get()) : nullptr;
    if (!master || uid.empty())
      return {};
    for (auto* c = icalcomponent_get_first_component(root.get(), ICAL_VTODO_COMPONENT); c;
         c = icalcomponent_get_next_component(root.get(), ICAL_VTODO_COMPONENT))
      replace(c, ICAL_UID_PROPERTY, icalproperty_new_uid(uid.c_str()));
    return serialize(root.get());
  }
  std::string editTask(
      const std::string& source, const TaskItem& displayed, const TaskDraft* draft, std::optional<bool> completed
  ) {
    auto root = parse(source);
    auto* c = root ? todo(root.get()) : nullptr;
    if (!c)
      return {};
    updateFields(c, displayed, draft, completed);
    return serialize(root.get());
  }
  bool equivalent(const std::string& a, const std::string& b) {
    if (a == b)
      return true;
    auto x = parse(a), y = parse(b);
    return x && y && canonical(x.get()) == canonical(y.get());
  }
  std::string randomId() {
    std::array<unsigned char, 16> bytes;
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 33> hex;
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
  }
  std::string accountId(const std::string& url, const std::string& username) {
    const auto identity = url + '\n' + username;
    std::array<unsigned char, 32> hash;
    crypto_generichash(
        hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(identity.data()), identity.size(), nullptr, 0
    );
    std::array<char, 65> hex;
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    return hex.data();
  }
} // namespace tasks
