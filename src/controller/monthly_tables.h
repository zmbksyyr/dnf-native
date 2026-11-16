// Original auction SQL needs both history families in gold/cera databases.
// Own their current/next month schemas instead of cloning the stale seed tables.
#pragma once
#include "resource_zip.h"
#include <map>
#include <sstream>

static std::wstring monthly_prepare(const std::wstring &exe, const std::wstring &instance, bool repair) {
    auto query = [&](const std::wstring &text) {
        ToolResult r = sql(exe, instance, text.c_str());
        if (r.code) throw std::runtime_error("Monthly table SQL failed; private log retained");
        r.output.erase(std::remove(r.output.begin(), r.output.end(), '\r'), r.output.end());
        return r.output;
    };
    std::string schemas = query(L"SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME IN ('taiwan_cain_auction_gold','taiwan_cain_auction_cera','taiwan_cain_log');");
    if (schemas == "0\n") return {}; // Plain MySQL initialization, before game seed import.
    if (schemas != "3\n") throw std::runtime_error("Game monthly schemas are incomplete");
    std::string dates = query(L"SELECT DATE_FORMAT(NOW(),'%Y%m'),DATE_FORMAT(DATE_ADD(NOW(),INTERVAL 1 MONTH),'%Y%m');");
    if (dates.size() != 14 || dates[6] != '\t' || dates[13] != '\n' ||
        dates.substr(0,6).find_first_not_of("0123456789") != std::string::npos ||
        dates.substr(7,6).find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Unexpected database monthly date");
    const std::wstring months[] = {decode(dates.substr(0,6)), decode(dates.substr(7,6))};
    using Columns = std::vector<std::pair<std::wstring,std::wstring>>;
    const Columns history = {
        {L"auction_id",L"BIGINT UNSIGNED NOT NULL PRIMARY KEY"}, {L"start_time",L"DATETIME NULL"},
        {L"occ_time",L"DATETIME NULL"}, {L"event_type",L"TINYINT NULL"}, {L"owner_id",L"INT NULL"},
        {L"buyer_id",L"INT NULL"}, {L"price",L"BIGINT NULL"}, {L"seal_flag",L"TINYINT NULL"},
        {L"item_id",L"INT UNSIGNED NULL"}, {L"add_info",L"INT NULL"}, {L"upgrade",L"TINYINT UNSIGNED NULL"},
        {L"seal_cnt",L"TINYINT UNSIGNED NULL"}, {L"endurance",L"SMALLINT UNSIGNED NULL"},
        {L"extend_info",L"INT UNSIGNED NULL"}, {L"owner_postal_id",L"INT UNSIGNED NULL"},
        {L"buyer_postal_id",L"INT UNSIGNED NULL"}, {L"amplify_option",L"TINYINT UNSIGNED NOT NULL DEFAULT 0"},
        {L"amplify_value",L"MEDIUMINT UNSIGNED NOT NULL DEFAULT 0"}, {L"item_guid",L"VARBINARY(10) NULL"},
        {L"unit_price",L"INT UNSIGNED NOT NULL DEFAULT 0"}, {L"roi_high_key",L"BIGINT NOT NULL DEFAULT 0"},
        {L"roi_low_key",L"INT NOT NULL DEFAULT 0"}, {L"commission",L"INT UNSIGNED NOT NULL DEFAULT 0"},
        {L"owner_type",L"TINYINT UNSIGNED NOT NULL DEFAULT 0"}, {L"random_option",L"VARBINARY(14) NULL"},
        {L"seperate_upgrade",L"TINYINT UNSIGNED NOT NULL DEFAULT 0"}
    };
    const Columns buyer = {
        {L"auction_id",L"BIGINT UNSIGNED NULL"}, {L"occ_time",L"DATETIME NULL"},
        {L"pre_buyer_id",L"INT NULL"}, {L"buyer_id",L"INT NULL"}, {L"pre_price",L"BIGINT NULL"},
        {L"price",L"BIGINT NULL"}, {L"pre_buyer_postal_id",L"INT UNSIGNED NULL"},
        {L"commission",L"INT UNSIGNED NOT NULL DEFAULT 0"}
    };
    const Columns response = {
        {L"channel_no",L"INT NOT NULL"}, {L"occ_time",L"DATETIME NOT NULL"},
        {L"packet_id",L"INT NOT NULL"}, {L"packet_count",L"BIGINT UNSIGNED NOT NULL"},
        {L"total_response_time",L"BIGINT UNSIGNED NOT NULL"}, {L"avg_response_time",L"BIGINT UNSIGNED NOT NULL"}
    };
    auto fields = [&](const std::wstring &schema, const std::wstring &table) {
        std::map<std::wstring,bool> result;
        std::istringstream rows(query(L"SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + schema + L"' AND TABLE_NAME='" + table + L"';"));
        std::string line;
        while (std::getline(rows,line)) result[decode(line)] = true;
        return result;
    };
    auto create = [&](const std::wstring &name, const Columns &columns, const std::wstring &index) {
        std::wstring text = L"CREATE TABLE " + name + L" (";
        for (const auto &col : columns) text += L"`" + col.first + L"` " + col.second + L",";
        if (index.empty()) text.pop_back(); else text += index;
        return text + L") ENGINE=InnoDB DEFAULT CHARSET=utf8;";
    };
    unsigned created = 0, migrated = 0;
    for (const auto &month : months) {
        for (const wchar_t *db : {L"taiwan_cain_auction_gold",L"taiwan_cain_auction_cera",L"taiwan_cain_log"}) {
            std::wstring schema = db;
            for (unsigned family = 0; family < (schema == L"taiwan_cain_log" ? 1u : 2u); ++family) {
                bool log = schema == L"taiwan_cain_log";
                std::wstring table = (log ? L"log_response_time_" : family ? L"auction_history_buyer_" : L"auction_history_") + month;
                std::wstring name = schema + L"." + table;
                const Columns &columns = log ? response : family ? buyer : history;
                std::wstring index = log ? L"KEY idx_occ_time(occ_time)" : family ? L"KEY idx_auction_id(auction_id),KEY idx_buyer_id(buyer_id)" : L"";
                auto existing = fields(schema,table);
                if (existing.empty()) { query(create(name,columns,index)); ++created; continue; }
                std::wstring additions;
                for (const auto &col : columns) if (!existing.count(col.first))
                    additions += (additions.empty() ? L"" : L",") + std::wstring(L"ADD COLUMN `") + col.first + L"` " + col.second;
                if (additions.empty()) continue;
                if (!repair) throw std::runtime_error("Monthly schema needs repair; stop game/auction/point and restart the database from the controller");
                // Additive auction migration is atomic per ALTER. Keep a real
                // official SQL backup before touching any existing table.
                std::wstring relative = L".mysql\\monthly-" + unique_name() + L".private.sql";
                std::wstring backup = instance + L"\\" + relative;
                write_private(backup, "");
                ToolResult dump = run_tool(exe, instance, {L"mysqldump",instance,L"--defaults-file=client.private.cnf",
                    L"--lock-tables",L"--hex-blob",L"--skip-comments",L"--set-gtid-purged=OFF",
                    L"--result-file=" + relative,schema,table},180000);
                if (dump.code) throw std::runtime_error("Monthly table backup failed; no schema change applied to that table");
                std::string original = read_file(backup);
                if (original.find("CREATE TABLE `" + std::string(table.begin(),table.end()) + "`") == std::string::npos)
                    throw std::runtime_error("Monthly table backup is incomplete; schema unchanged");
                std::string hash = file_sha256(backup);
                write_private(backup + L".sha256",hash + "\n");
                std::wstring suffix = L"_native_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
                std::wstring fresh = name + suffix, old = name + suffix + L"_old";
                std::string rollback = "-- Stop game/auction/point before rollback. Migrated data is retained under a separate name.\n";
                if (log) {
                    // The seed mislabels a different statistics schema. Retain
                    // its rows and keys intact, then atomically exchange names.
                    rollback += "RENAME TABLE " +
                        std::string(name.begin(),name.end()) + " TO " + std::string(fresh.begin(),fresh.end()) + "," +
                        std::string(old.begin(),old.end()) + " TO " + std::string(name.begin(),name.end()) + ";\n";
                } else {
                    // The seed's identity/key columns must be present. Never
                    // manufacture an order primary key for an unknown layout.
                    if (!existing.count(L"auction_id") || !existing.count(L"occ_time"))
                        throw std::runtime_error("Unknown auction history layout; backup retained, schema unchanged");
                    rollback += "USE " + std::string(schema.begin(),schema.end()) + ";\nRENAME TABLE " +
                        std::string(name.begin(),name.end()) + " TO " + std::string(fresh.begin(),fresh.end()) + ";\n" + original;
                }
                std::wstring rollback_file = backup + L".rollback.sql";
                write_private(rollback_file,rollback);
                bool changed = false;
                try {
                    if (log) {
                        query(create(fresh,columns,index));
                        query(L"RENAME TABLE " + name + L" TO " + old + L"," + fresh + L" TO " + name + L";");
                    } else query(L"ALTER TABLE " + name + L" " + additions + L";");
                    changed = true;
                    auto after = fields(schema,table);
                    for (const auto &col : columns) if (!after.count(col.first))
                        throw std::runtime_error("Monthly table verification failed");
                } catch (...) {
                    if (changed) {
                        ToolResult restored = run_tool(exe,instance,{L"mysql",instance,L"--defaults-file=client.private.cnf"},180000,rollback_file);
                        if (restored.code) throw std::runtime_error("Monthly migration and rollback failed; backup, recovery SQL and private logs retained");
                    }
                    throw;
                }
                ++migrated;
            }
        }
    }
    return L"月表已核对：" + months[0] + L" / " + months[1] + L"；新建 " + std::to_wstring(created) +
           L"，修正 " + std::to_wstring(migrated) + L"（原表备份保留在 .mysql）";
}
