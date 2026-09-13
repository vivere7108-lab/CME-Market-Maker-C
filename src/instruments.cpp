#include "harvester/instruments.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace harvester {

const Product ES{"ES", "ES", "ES", "CME", "USD", 50.0, 0.25, 2.30, 34'500.0, "America/Chicago", {"ES", "EMINI"}};
const Product MES{"MES", "MES", "MES", "CME", "USD", 5.0, 0.25, 0.62, 3'450.0, "America/Chicago", {"MES", "MICRO-ES"}};
const Product NQ{"NQ", "NQ", "NQ", "CME", "USD", 20.0, 0.25, 2.30, 40'000.0, "America/Chicago", {"NQ"}};
const Product MNQ{"MNQ", "MNQ", "MNQ", "CME", "USD", 2.0, 0.25, 0.62, 4'000.0, "America/Chicago", {"MNQ"}};

namespace {

std::string upper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

std::string strip(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::map<std::string, Product>& registry() {
    static std::map<std::string, Product> products = {
        {"ES", ES}, {"MES", MES}, {"NQ", NQ}, {"MNQ", MNQ},
    };
    return products;
}

}  // namespace

void register_product(const Product& product) {
    const std::lock_guard<std::mutex> guard(registry_mutex());
    registry()[upper(product.name)] = product;
}

const Product& get_product(std::string_view name) {
    const std::lock_guard<std::mutex> guard(registry_mutex());
    const std::string key = upper(strip(name));
    auto& products = registry();
    if (const auto it = products.find(key); it != products.end()) return it->second;
    for (const auto& [_, product] : products) {
        for (const auto& alias : product.aliases) {
            if (upper(alias) == key) return product;
        }
    }
    std::string known;
    for (const auto& [n, _] : products) {
        if (!known.empty()) known += ", ";
        known += n;
    }
    throw UnknownProduct("unknown product '" + std::string(name) + "'; registered: " + (known.empty() ? "<none>" : known));
}

std::vector<std::string> registered_products() {
    const std::lock_guard<std::mutex> guard(registry_mutex());
    std::vector<std::string> names;
    for (const auto& [n, _] : registry()) names.push_back(n);
    return names;
}

}  // namespace harvester
