#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

const std::filesystem::path DATA_DIRECTORY = "data";
const std::filesystem::path DATA_FILE = DATA_DIRECTORY / "transactions.txt";
const std::filesystem::path TEMP_FILE = DATA_DIRECTORY / "transactions.tmp";
const std::filesystem::path WEB_DIRECTORY = "web";
const int SERVER_PORT = 8080;
const std::size_t MAX_REQUEST_SIZE = 1024 * 1024;

#ifdef _WIN32
using Socket = SOCKET;
const Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
const Socket INVALID_SOCKET_VALUE = -1;
void closeSocket(Socket socket) { close(socket); }
#endif

std::string trim(const std::string& value) {
    const std::string whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

bool isLeapYear(int year) {
    return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

bool isValidDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) {
            return false;
        }
    }

    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));
    if (year < 1 || month < 1 || month > 12) {
        return false;
    }
    const int days[] = {
        31, isLeapYear(year) ? 29 : 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };
    return day >= 1 && day <= days[month - 1];
}

std::string currentDate() {
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (local == nullptr) {
        throw std::runtime_error("Could not read the computer's local date.");
    }
    std::ostringstream result;
    result << std::put_time(local, "%Y-%m-%d");
    return result.str();
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character) << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string formatAmount(double amount) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << amount;
    return output.str();
}

class Transaction {
public:
    Transaction(int id, std::string date, std::string category,
                std::string description, double amount)
        : id_(id), date_(std::move(date)), category_(std::move(category)),
          description_(std::move(description)), amount_(amount) {}
    virtual ~Transaction() = default;

    int id() const { return id_; }
    const std::string& date() const { return date_; }
    const std::string& category() const { return category_; }
    const std::string& description() const { return description_; }
    double amount() const { return amount_; }

    virtual std::string type() const = 0;
    virtual double signedAmount() const = 0;

private:
    int id_;
    std::string date_;
    std::string category_;
    std::string description_;
    double amount_;
};

class IncomeTransaction final : public Transaction {
public:
    using Transaction::Transaction;
    std::string type() const override { return "Income"; }
    double signedAmount() const override { return amount(); }
};

class ExpenseTransaction final : public Transaction {
public:
    using Transaction::Transaction;
    std::string type() const override { return "Expense"; }
    double signedAmount() const override { return -amount(); }
};

using TransactionList = std::vector<std::unique_ptr<Transaction>>;

std::unique_ptr<Transaction> makeTransaction(int id, const std::string& date,
                                             const std::string& type,
                                             const std::string& category,
                                             const std::string& description,
                                             double amount) {
    if (type == "Income") {
        return std::make_unique<IncomeTransaction>(
            id, date, category, description, amount);
    }
    if (type == "Expense") {
        return std::make_unique<ExpenseTransaction>(
            id, date, category, description, amount);
    }
    throw std::invalid_argument("Transaction type must be Income or Expense.");
}

struct LoadResult {
    TransactionList transactions;
    int invalidRows = 0;
};

bool parseTransaction(const std::string& line, std::unique_ptr<Transaction>& result) {
    std::istringstream input(line);
    std::string idText;
    std::string date;
    std::string type;
    std::string category;
    std::string description;
    std::string amountText;
    std::string extra;

    if (!std::getline(input, idText, '|') ||
        !std::getline(input, date, '|') ||
        !std::getline(input, type, '|') ||
        !std::getline(input, category, '|') ||
        !std::getline(input, description, '|') ||
        !std::getline(input, amountText, '|') ||
        std::getline(input, extra, '|')) {
        return false;
    }

    try {
        std::size_t idLength = 0;
        std::size_t amountLength = 0;
        const int id = std::stoi(idText, &idLength);
        const double amount = std::stod(amountText, &amountLength);
        if (idLength != idText.size() || amountLength != amountText.size() ||
            id <= 0 || !isValidDate(date) || trim(category).empty() ||
            trim(description).empty() || !std::isfinite(amount) || amount <= 0.0 ||
            category.find('|') != std::string::npos ||
            description.find('|') != std::string::npos) {
            return false;
        }
        result = makeTransaction(id, date, type, category, description, amount);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

class TransactionStore {
public:
    virtual ~TransactionStore() = default;
    virtual LoadResult load() const = 0;
    virtual bool append(const Transaction& transaction) const = 0;
    virtual bool replace(const TransactionList& transactions) const = 0;
};

class TextFileTransactionStore final : public TransactionStore {
public:
    explicit TextFileTransactionStore(std::filesystem::path path)
        : path_(std::move(path)) {}

    LoadResult load() const override {
        LoadResult result;
        std::ifstream file(path_);
        if (!file) {
            if (std::filesystem::exists(path_)) {
                throw std::runtime_error("Could not open " + path_.string());
            }
            return result;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (trim(line).empty()) {
                continue;
            }
            std::unique_ptr<Transaction> transaction;
            if (parseTransaction(line, transaction)) {
                result.transactions.push_back(std::move(transaction));
            } else {
                ++result.invalidRows;
            }
        }
        if (file.bad()) {
            throw std::runtime_error("Error reading " + path_.string());
        }
        return result;
    }

    bool append(const Transaction& transaction) const override {
        std::ofstream file(path_, std::ios::app);
        if (!file) {
            return false;
        }
        writeRow(file, transaction);
        return static_cast<bool>(file);
    }

    bool replace(const TransactionList& transactions) const override {
        {
            std::ofstream file(TEMP_FILE, std::ios::trunc);
            if (!file) {
                return false;
            }
            for (const auto& transaction : transactions) {
                writeRow(file, *transaction);
            }
            if (!file) {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(TEMP_FILE, ignored);
                return false;
            }
        }

        std::error_code error;
        std::filesystem::copy_file(
            TEMP_FILE, path_, std::filesystem::copy_options::overwrite_existing, error);
        std::error_code cleanupError;
        std::filesystem::remove(TEMP_FILE, cleanupError);
        return !error;
    }

private:
    static void writeRow(std::ostream& output, const Transaction& transaction) {
        output << transaction.id() << '|' << transaction.date() << '|'
               << transaction.type() << '|' << transaction.category() << '|'
               << transaction.description() << '|'
               << formatAmount(transaction.amount()) << '\n';
    }

    std::filesystem::path path_;
};

struct JsonValue {
    std::string value;
    bool isString = false;
};

void appendUtf8(std::string& output, unsigned int codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

class JsonObjectParser {
public:
    explicit JsonObjectParser(const std::string& source) : source_(source) {}

    std::unordered_map<std::string, JsonValue> parse() {
        std::unordered_map<std::string, JsonValue> result;
        skipWhitespace();
        expect('{');
        skipWhitespace();
        if (take('}')) {
            return result;
        }
        while (position_ < source_.size()) {
            skipWhitespace();
            const std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            JsonValue value;
            if (peek() == '"') {
                value.isString = true;
                value.value = parseString();
            } else {
                const std::size_t start = position_;
                while (position_ < source_.size() &&
                       source_[position_] != ',' && source_[position_] != '}' &&
                       source_[position_] != ' ' && source_[position_] != '\n' &&
                       source_[position_] != '\r' && source_[position_] != '\t') {
                    ++position_;
                }
                if (start == position_) {
                    throw std::invalid_argument("Expected a JSON string or number.");
                }
                value.value = source_.substr(start, position_ - start);
            }
            result[key] = std::move(value);
            skipWhitespace();
            if (take('}')) {
                skipWhitespace();
                if (position_ != source_.size()) {
                    throw std::invalid_argument("Unexpected content after JSON object.");
                }
                return result;
            }
            expect(',');
        }
        throw std::invalid_argument("Unfinished JSON object.");
    }

private:
    char peek() const {
        return position_ < source_.size() ? source_[position_] : '\0';
    }

    bool take(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void expect(char expected) {
        if (!take(expected)) {
            throw std::invalid_argument(std::string("Expected '") + expected + "' in JSON.");
        }
    }

    void skipWhitespace() {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') {
            ++position_;
        }
    }

    std::string parseString() {
        expect('"');
        std::string result;
        while (position_ < source_.size()) {
            const char character = source_[position_++];
            if (character == '"') {
                return result;
            }
            if (character != '\\') {
                if (static_cast<unsigned char>(character) < 0x20) {
                    throw std::invalid_argument("Control character in JSON string.");
                }
                result.push_back(character);
                continue;
            }
            if (position_ == source_.size()) {
                break;
            }
            const char escaped = source_[position_++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (position_ + 4 > source_.size()) {
                        throw std::invalid_argument("Invalid Unicode escape in JSON.");
                    }
                    unsigned int codePoint = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const char hex = source_[position_++];
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9') codePoint += hex - '0';
                        else if (hex >= 'a' && hex <= 'f') codePoint += hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F') codePoint += hex - 'A' + 10;
                        else throw std::invalid_argument("Invalid Unicode escape in JSON.");
                    }
                    appendUtf8(result, codePoint);
                    break;
                }
                default: throw std::invalid_argument("Invalid escape in JSON string.");
            }
        }
        throw std::invalid_argument("Unclosed JSON string.");
    }

    const std::string& source_;
    std::size_t position_ = 0;
};

const JsonValue& requiredField(
    const std::unordered_map<std::string, JsonValue>& fields,
    const std::string& name, bool expectString) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->second.isString != expectString) {
        throw std::invalid_argument("Missing or invalid field: " + name);
    }
    return found->second;
}

std::string transactionJson(const Transaction& transaction) {
    std::ostringstream output;
    output << "{\"id\":" << transaction.id()
           << ",\"date\":\"" << jsonEscape(transaction.date())
           << "\",\"type\":\"" << transaction.type()
           << "\",\"category\":\"" << jsonEscape(transaction.category())
           << "\",\"description\":\"" << jsonEscape(transaction.description())
           << "\",\"amount\":" << formatAmount(transaction.amount()) << '}';
    return output.str();
}

std::string transactionsJson(const LoadResult& data) {
    std::ostringstream output;
    output << "{\"invalidRows\":" << data.invalidRows << ",\"transactions\":[";
    for (std::size_t i = 0; i < data.transactions.size(); ++i) {
        if (i != 0) output << ',';
        output << transactionJson(*data.transactions[i]);
    }
    output << "]}";
    return output.str();
}

std::string monthLabel(const std::tm& date) {
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%b", &date);
    return buffer;
}

std::string summaryJson(const TransactionList& transactions) {
    const std::string month = currentDate().substr(0, 7);
    double balance = 0.0;
    double income = 0.0;
    double expenses = 0.0;
    double monthIncome = 0.0;
    double monthExpenses = 0.0;
    std::map<std::string, std::pair<double, double>> monthly;
    std::map<std::string, double> categories;

    const std::time_t now = std::time(nullptr);
    std::tm monthDate = *std::localtime(&now);
    monthDate.tm_mday = 1;
    monthDate.tm_hour = 12;

    std::vector<std::string> monthKeys;
    for (int offset = 5; offset >= 0; --offset) {
        std::tm date = monthDate;
        date.tm_mon -= offset;
        std::mktime(&date);
        std::ostringstream key;
        key << std::put_time(&date, "%Y-%m");
        monthKeys.push_back(key.str());
        monthly[key.str()] = {0.0, 0.0};
    }

    for (const auto& item : transactions) {
        const bool isIncome = item->type() == "Income";
        const std::string transactionMonth = item->date().substr(0, 7);
        if (isIncome) {
            income += item->amount();
            balance += item->signedAmount();
        } else {
            expenses += item->amount();
            balance += item->signedAmount();
            categories[item->category()] += item->amount();
        }
        const auto monthEntry = monthly.find(transactionMonth);
        if (monthEntry != monthly.end()) {
            (isIncome ? monthEntry->second.first : monthEntry->second.second) += item->amount();
        }
        if (transactionMonth == month) {
            (isIncome ? monthIncome : monthExpenses) += item->amount();
        }
    }

    std::vector<std::pair<std::string, double>> sortedCategories(categories.begin(), categories.end());
    std::sort(sortedCategories.begin(), sortedCategories.end(),
              [](const auto& left, const auto& right) { return left.second > right.second; });

    std::ostringstream output;
    output << "{\"balance\":" << formatAmount(balance)
           << ",\"income\":" << formatAmount(income)
           << ",\"expenses\":" << formatAmount(expenses)
           << ",\"monthIncome\":" << formatAmount(monthIncome)
           << ",\"monthExpenses\":" << formatAmount(monthExpenses)
           << ",\"month\":\"" << month << "\",\"transactionCount\":"
           << transactions.size() << ",\"monthly\":[";
    for (std::size_t i = 0; i < monthKeys.size(); ++i) {
        if (i != 0) output << ',';
        std::tm date = monthDate;
        date.tm_mon -= static_cast<int>(monthKeys.size() - i - 1);
        std::mktime(&date);
        const auto totals = monthly[monthKeys[i]];
        output << "{\"month\":\"" << monthLabel(date)
               << "\",\"income\":" << formatAmount(totals.first)
               << ",\"expenses\":" << formatAmount(totals.second) << '}';
    }
    output << "],\"categories\":[";
    for (std::size_t i = 0; i < sortedCategories.size(); ++i) {
        if (i != 0) output << ',';
        output << "{\"name\":\"" << jsonEscape(sortedCategories[i].first)
               << "\",\"amount\":" << formatAmount(sortedCategories[i].second) << '}';
    }
    output << "]}";
    return output.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::string body;
};

bool receiveRequest(Socket socket, HttpRequest& request) {
    std::string raw;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    std::size_t expectedSize = 0;

    while (raw.size() <= MAX_REQUEST_SIZE) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::istringstream headers(raw.substr(0, headerEnd));
                std::string line;
                if (!std::getline(headers, line)) return false;
                std::istringstream firstLine(line);
                std::string version;
                if (!(firstLine >> request.method >> request.target >> version) ||
                    version.rfind("HTTP/", 0) != 0) {
                    return false;
                }
                std::size_t contentLength = 0;
                while (std::getline(headers, line)) {
                    const auto separator = line.find(':');
                    if (separator == std::string::npos) continue;
                    if (lower(trim(line.substr(0, separator))) == "content-length") {
                        try {
                            contentLength = static_cast<std::size_t>(
                                std::stoul(trim(line.substr(separator + 1))));
                        } catch (const std::exception&) {
                            return false;
                        }
                    }
                }
                if (contentLength > MAX_REQUEST_SIZE ||
                    headerEnd + 4 + contentLength > MAX_REQUEST_SIZE) {
                    return false;
                }
                expectedSize = headerEnd + 4 + contentLength;
            }
        }
        if (headerEnd != std::string::npos && raw.size() >= expectedSize) {
            request.body = raw.substr(headerEnd + 4, expectedSize - headerEnd - 4);
            return true;
        }
    }
    return false;
}

bool sendAll(Socket socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const int count = send(socket, data.data() + sent,
                               static_cast<int>(data.size() - sent), MSG_NOSIGNAL);
#else
        const int count = send(socket, data.data() + sent,
                               static_cast<int>(data.size() - sent), 0);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void sendResponse(Socket socket, int status, const std::string& statusText,
                  const std::string& contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << statusText << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n\r\n" << body;
    sendAll(socket, response.str());
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not read " + path.string());
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

class FinTrackServer {
public:
    explicit FinTrackServer(const TransactionStore& store) : store_(store) {}

    void serve(Socket client) const {
        HttpRequest request;
        if (!receiveRequest(client, request)) {
            sendResponse(client, 400, "Bad Request", "application/json",
                         "{\"error\":\"Invalid or oversized HTTP request.\"}");
            return;
        }

        const std::string path = request.target.substr(0, request.target.find('?'));
        try {
            if (request.method == "GET" && path == "/api/transactions") {
                sendResponse(client, 200, "OK", "application/json; charset=utf-8",
                             transactionsJson(store_.load()));
            } else if (request.method == "GET" && path == "/api/summary") {
                const LoadResult data = store_.load();
                sendResponse(client, 200, "OK", "application/json; charset=utf-8",
                             summaryJson(data.transactions));
            } else if (request.method == "POST" && path == "/api/transactions") {
                createTransaction(client, request.body);
            } else if (request.method == "DELETE" &&
                       path.rfind("/api/transactions/", 0) == 0) {
                deleteTransaction(client, path.substr(std::string("/api/transactions/").size()));
            } else if (request.method == "GET") {
                serveStatic(client, path);
            } else {
                sendResponse(client, 405, "Method Not Allowed", "application/json",
                             "{\"error\":\"Method not allowed.\"}");
            }
        } catch (const std::invalid_argument& error) {
            sendResponse(client, 400, "Bad Request", "application/json; charset=utf-8",
                         "{\"error\":\"" + jsonEscape(error.what()) + "\"}");
        } catch (const std::exception& error) {
            std::cerr << "Request error: " << error.what() << '\n';
            sendResponse(client, 500, "Internal Server Error", "application/json; charset=utf-8",
                         "{\"error\":\"The server could not complete the request.\"}");
        }
    }

private:
    void createTransaction(Socket client, const std::string& body) const {
        const auto fields = JsonObjectParser(body).parse();
        const std::string date = requiredField(fields, "date", true).value;
        const std::string type = requiredField(fields, "type", true).value;
        const std::string category = trim(requiredField(fields, "category", true).value);
        const std::string description = trim(requiredField(fields, "description", true).value);
        const std::string amountText = requiredField(fields, "amount", false).value;

        double amount = 0.0;
        try {
            std::size_t charactersRead = 0;
            amount = std::stod(amountText, &charactersRead);
            if (charactersRead != amountText.size()) {
                throw std::invalid_argument("Amount must be a number.");
            }
        } catch (const std::exception&) {
            throw std::invalid_argument("Amount must be a valid number.");
        }

        if (!isValidDate(date)) {
            throw std::invalid_argument("Choose a valid date in YYYY-MM-DD format.");
        }
        if (type != "Income" && type != "Expense") {
            throw std::invalid_argument("Choose Income or Expense.");
        }
        if (category.empty() || description.empty() ||
            category.find('|') != std::string::npos ||
            description.find('|') != std::string::npos) {
            throw std::invalid_argument("Category and description are required and cannot contain |.");
        }
        if (!std::isfinite(amount) || amount <= 0.0) {
            throw std::invalid_argument("Amount must be greater than zero.");
        }

        LoadResult data = store_.load();
        int nextId = 1;
        for (const auto& transaction : data.transactions) {
            nextId = std::max(nextId, transaction->id() + 1);
        }
        auto transaction = makeTransaction(
            nextId, date, type, category, description, amount);
        if (!store_.append(*transaction)) {
            throw std::runtime_error("Could not save the transaction.");
        }
        sendResponse(client, 201, "Created", "application/json; charset=utf-8",
                     transactionJson(*transaction));
    }

    void deleteTransaction(Socket client, const std::string& idText) const {
        int id = 0;
        try {
            std::size_t charactersRead = 0;
            id = std::stoi(idText, &charactersRead);
            if (charactersRead != idText.size() || id <= 0) {
                throw std::invalid_argument("Invalid transaction ID.");
            }
        } catch (const std::exception&) {
            throw std::invalid_argument("Invalid transaction ID.");
        }

        LoadResult data = store_.load();
        if (data.invalidRows != 0) {
            throw std::invalid_argument("Fix invalid rows in data/transactions.txt before deleting.");
        }
        const auto found = std::find_if(
            data.transactions.begin(), data.transactions.end(),
            [id](const auto& transaction) { return transaction->id() == id; });
        if (found == data.transactions.end()) {
            sendResponse(client, 404, "Not Found", "application/json",
                         "{\"error\":\"Transaction not found.\"}");
            return;
        }
        data.transactions.erase(found);
        if (!store_.replace(data.transactions)) {
            throw std::runtime_error("Could not update the transactions file.");
        }
        sendResponse(client, 200, "OK", "application/json", "{\"deleted\":true}");
    }

    void serveStatic(Socket client, const std::string& path) const {
        std::string filename;
        std::string contentType;
        if (path == "/" || path == "/index.html") {
            filename = "index.html";
            contentType = "text/html; charset=utf-8";
        } else if (path == "/styles.css") {
            filename = "styles.css";
            contentType = "text/css; charset=utf-8";
        } else if (path == "/app.js") {
            filename = "app.js";
            contentType = "text/javascript; charset=utf-8";
        } else {
            sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8",
                         "Not found");
            return;
        }
        sendResponse(client, 200, "OK", contentType,
                     readFile(WEB_DIRECTORY / filename));
    }

    const TransactionStore& store_;
};

bool startSocketLibrary() {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void stopSocketLibrary() {
#ifdef _WIN32
    WSACleanup();
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc > 2) {
            throw std::invalid_argument("Usage: fintrack [port]");
        }
        int port = SERVER_PORT;
        if (argc == 2) {
            std::size_t charactersRead = 0;
            port = std::stoi(argv[1], &charactersRead);
            if (charactersRead != std::string(argv[1]).size() || port < 1 || port > 65535) {
                throw std::invalid_argument("Port must be a number from 1 to 65535.");
            }
        }
        std::filesystem::create_directories(DATA_DIRECTORY);
        if (!std::filesystem::exists(WEB_DIRECTORY / "index.html")) {
            throw std::runtime_error("web/index.html is missing. Run FinTrack from its project folder.");
        }
        if (!startSocketLibrary()) {
            throw std::runtime_error("Could not initialize the network library.");
        }

        const Socket server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == INVALID_SOCKET_VALUE) {
            stopSocketLibrary();
            throw std::runtime_error("Could not create the local web server socket.");
        }
        int reuseAddress = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            closeSocket(server);
            stopSocketLibrary();
            throw std::runtime_error(
                "Could not start on port " + std::to_string(port) +
                ". Close another app using that port or choose a different port.");
        }
        if (listen(server, 16) < 0) {
            closeSocket(server);
            stopSocketLibrary();
            throw std::runtime_error("Could not listen for browser connections.");
        }

        std::cout << "FinTrack is running at http://127.0.0.1:" << port << '\n'
                  << "Keep this window open while using the dashboard.\n"
                  << "Press Ctrl+C here to stop the server.\n";
        const TextFileTransactionStore store(DATA_FILE);
        const FinTrackServer application(store);
        while (true) {
            const Socket client = accept(server, nullptr, nullptr);
            if (client == INVALID_SOCKET_VALUE) {
                std::cerr << "Could not accept a browser connection.\n";
                continue;
            }
            application.serve(client);
            closeSocket(client);
        }
    } catch (const std::exception& error) {
        std::cerr << "FinTrack: " << error.what() << '\n';
        return 1;
    }
}
