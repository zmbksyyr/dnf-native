-- LuaSQL work is loaded inside the worker: the shipped luv crashes while
-- dumping this larger function on both original Linux and Windows hosts.
local database_work = function(encoded)
    local json = require('json')
    local cfg = json.decode(encoded)
    package.cpath = '/dp2/lua/?.so;' .. package.cpath
    local sql = require('luasql.mysql')
    local env, db
    local ok, result = pcall(function()
        env = assert(sql.mysql())
        db = assert(env:connect('d_taiwan', cfg.user, cfg.password, cfg.host, cfg.port))
        local function exec(text) return assert(db:execute(text)) end
        local function rows(text)
            local cursor = exec(text)
            local out, row = {}, cursor:fetch({}, 'a')
            while row do out[#out+1] = row; row = cursor:fetch({}, 'a') end
            cursor:close(); return out
        end
        local function token(value)
            assert(type(value)=='string' and #value==32 and value:match('^[0-9a-f]+$'), 'GM command token invalid')
            return "'" .. value .. "'"
        end
        -- Separate wire versions keep a previous coin-only bridge from ever
        -- interpreting a money command as SetCoin during rollback or upgrade.
        local queues = {
            {version=1, suffix='v1', field='coin', operations="'set_coin','add_coin','inspect'"},
            {version=2, suffix='v2', field='money', operations="'set_money','add_money'"},
        }
        if cfg.owner == '' then
            for _, queue in ipairs(queues) do
                exec(string.format([[CREATE TABLE IF NOT EXISTS dnf_gm_hosts_%s (
                    channel_no INT UNSIGNED PRIMARY KEY, owner CHAR(32) CHARACTER SET ascii NOT NULL,
                    seen DATETIME(6) NOT NULL) ENGINE=InnoDB]], queue.suffix))
                exec(string.format([[CREATE TABLE IF NOT EXISTS dnf_gm_online_%s (
                    token CHAR(32) CHARACTER SET ascii PRIMARY KEY,
                    m_id INT UNSIGNED NOT NULL, charac_no INT UNSIGNED NOT NULL,
                    operation ENUM(%s) NOT NULL, amount INT UNSIGNED NOT NULL,
                    state ENUM('pending','claimed','applied','done','rejected','unknown') NOT NULL DEFAULT 'pending',
                    owner CHAR(32) CHARACTER SET ascii NULL, before_value INT UNSIGNED NULL, after_value INT UNSIGNED NULL,
                    note VARCHAR(80) CHARACTER SET ascii NOT NULL DEFAULT '',
                    created DATETIME(6) NOT NULL, expires DATETIME(6) NOT NULL, updated DATETIME(6) NOT NULL,
                    INDEX pending_commands(state,created), INDEX account_commands(m_id,state,created)) ENGINE=InnoDB]],
                    queue.suffix, queue.operations))
            end
            return {owner=rows("SELECT REPLACE(UUID(),'-','') AS owner")[1].owner, commands={}}
        end
        local owner = token(cfg.owner)
        local commands = {}
        for _, queue in ipairs(queues) do
            local hosts, pending_table = 'dnf_gm_hosts_'..queue.suffix, 'dnf_gm_online_'..queue.suffix
            local other_table = 'dnf_gm_online_'..(queue.version==1 and 'v2' or 'v1')
            exec(string.format('INSERT IGNORE INTO %s VALUES(%d,%s,NOW(6))',hosts,cfg.channel,owner))
            exec(string.format([[UPDATE %s SET owner=%s,seen=NOW(6) WHERE channel_no=%d
                AND (owner=%s OR seen<DATE_SUB(NOW(6),INTERVAL 10 SECOND))]], hosts,owner,cfg.channel,owner))
            assert(rows(string.format('SELECT owner FROM %s WHERE channel_no=%d',hosts,cfg.channel))[1].owner==cfg.owner,
                   'another GM bridge owns this channel')
            for _, receipt in ipairs(cfg.receipts) do
                if (receipt.version or 1)==queue.version then
                    assert(receipt.state=='applied' or receipt.state=='rejected' or receipt.state=='unknown' or receipt.state=='done', 'GM receipt state invalid')
                    assert(receipt.note:match('^[a-z_]+$'), 'GM receipt note invalid')
                    local values = ''
                    if receipt.before then
                        values = string.format(',before_value=%u,after_value=%u', receipt.before, receipt.after)
                    end
                    if receipt.charac_no then values=values..string.format(',charac_no=%u',receipt.charac_no) end
                    exec(string.format([[UPDATE %s SET state='%s',note='%s',updated=NOW(6)%s
                        WHERE token=%s AND owner=%s AND state='claimed']],pending_table,receipt.state,receipt.note,values,token(receipt.token),owner))
                end
            end
            -- SaveMoneyCoin queues the original save. Confirm SQL separately, without
            -- reporting its enqueue result as persistence or replaying the command.
            exec(string.format([[UPDATE %s q JOIN taiwan_cain_2nd.inventory i ON i.charac_no=q.charac_no
                SET q.state='done',q.note='saved',q.updated=NOW(6)
                WHERE q.owner=%s AND q.state='applied' AND i.%s=q.after_value]],pending_table,owner,queue.field))
            exec(string.format([[UPDATE %s SET state='unknown',note='save_unconfirmed',updated=NOW(6)
                WHERE owner=%s AND state='applied' AND updated<DATE_SUB(NOW(6),INTERVAL 15 SECOND)]],pending_table,owner))
            -- Lost work and a previous host's commands are ambiguous, never pending
            -- again. End them explicitly so they cannot block this account forever.
            exec(string.format([[UPDATE %s q SET state='unknown',note='host_or_work_lost',updated=NOW(6)
                WHERE q.state IN ('claimed','applied') AND q.expires<NOW(6)
                AND NOT EXISTS (SELECT 1 FROM %s h WHERE h.owner=q.owner
                    AND h.seen>DATE_SUB(NOW(6),INTERVAL 10 SECOND))]],pending_table,hosts))
            exec(string.format([[UPDATE %s SET state='unknown',note='work_unconfirmed',updated=NOW(6)
                WHERE owner=%s AND state='claimed' AND expires<NOW(6)]],pending_table,owner))
            exec(string.format([[UPDATE %s SET state='rejected',note='expired',updated=NOW(6)
                WHERE state='pending' AND expires<NOW(6)]],pending_table))
            local pending = rows(string.format([[SELECT q.token,q.m_id,q.charac_no,q.operation,q.amount
                FROM %s q JOIN taiwan_login.login_account_3 l ON l.m_id=q.m_id
                WHERE q.state='pending' AND q.expires>NOW(6) AND l.login_status>=1 AND l.m_channel_no=%d
                AND NOT EXISTS (SELECT 1 FROM %s prior WHERE prior.m_id=q.m_id
                    AND (prior.state IN ('claimed','applied') OR (prior.state='pending'
                        AND (prior.created<q.created OR (prior.created=q.created AND prior.token<q.token)))))
                AND NOT EXISTS (SELECT 1 FROM %s other WHERE other.m_id=q.m_id AND other.state IN ('claimed','applied'))
                ORDER BY q.created LIMIT 32]],pending_table,cfg.channel,pending_table,other_table))
            for _, req in ipairs(pending) do
                local changed = exec(string.format([[UPDATE %s SET state='claimed',owner=%s,updated=NOW(6)
                    WHERE token=%s AND state='pending' AND expires>NOW(6)]],pending_table,owner,token(req.token)))
                if changed == 1 then req.version=queue.version; commands[#commands+1] = req end
            end
        end
        return {owner=cfg.owner, commands=commands}
    end)
    if db then db:close() end
    if env then env:close() end
    return json.encode({ok=ok,result=result})
end
if ...=='worker' then return database_work end

-- Online inventory commands for the pinned original game and its DP2 API.
-- Database I/O runs in luv workers. Only the completion callback calls game APIs.
local previous = rawget(_G, 'DNF_GM_ONLINE')
if previous then return previous end
local uv, json = require('luv'), require('json')
local ffi, game = require('ffi'), require('df.game')
local world, symbols = require('df.game.mgr.world'), require('df.game.symbols.core')
local input = assert(io.open('/dp2/gm-online.private.json', 'rb'), 'GM bridge configuration missing')
local config = json.decode(input:read('*a')); input:close()
assert(type(config.host) == 'string' and type(config.user) == 'string' and type(config.password) == 'string', 'GM database configuration invalid')
assert(math.type(config.port) == 'integer' and config.port > 0 and config.port <= 65535, 'GM database port invalid')
assert(math.type(config.channel) == 'integer' and config.channel > 0, 'GM channel invalid')
ffi.cdef[[
unsigned int DNF_GM_GetCoin(CInventory *);
void DNF_GM_SetCoin(CInventory *, unsigned int);
int DNF_GM_SaveMoneyCoin(CUser *);
void DNF_GM_SendCoin(CUser *);
unsigned int DNF_GM_GetMoney(CInventory *);
void DNF_GM_SetMoney(CInventory *, unsigned int);
int DNF_GM_MoneyLimit(CInventory *, int);
]]
symbols.alias('DNF_GM_GetCoin', '_ZNK10CInventory7GetCoinEv')
symbols.alias('DNF_GM_SetCoin', '_ZN10CInventory7SetCoinEj')
symbols.alias('DNF_GM_SaveMoneyCoin', '_ZN5CUser13SaveMoneyCoinEv')
symbols.alias('DNF_GM_SendCoin', '_ZN5CUser8SendCoinEv')
symbols.alias('DNF_GM_GetMoney', '_ZNK10CInventory9get_moneyEv')
symbols.alias('DNF_GM_SetMoney', '_ZN10CInventory9set_moneyEj')
symbols.alias('DNF_GM_MoneyLimit', '_ZN10CInventory13getValidMoneyEi')

local state = {owner='', receipts={}, busy=false}
local worker = uv.new_work(function(encoded)
    package.path='/dp2/lua/?.lua;/dp2/lua/?/init.lua;'..package.path
    return assert(loadfile('/dp2/dnf-gm-online.lua'))('worker')(encoded)
end,function(encoded)
    state.busy = false
    if state.stopping then return end
    local decoded = json.decode(encoded)
    if not decoded.ok then
        -- Keep unacknowledged receipts. A later worker can repeat only the SQL
        -- receipt update, never the original game operation.
        if state.error ~= decoded.result then io.stderr:write('GM bridge: ',tostring(decoded.result),'\n') end
        state.error = decoded.result
        return
    end
    state.error = nil
    state.owner = decoded.result.owner
    state.receipts = {}
    for _, req in ipairs(decoded.result.commands) do
        local receipt = {token=req.token,version=req.version,state='rejected',note='wrong_active_character'}
        local ptr = world.FindUserByAcc(tonumber(req.m_id))
        local user = ptr and game.fac.user(ptr)
        local money = req.version==2 and (req.operation=='set_money' or req.operation=='add_money')
        local coin = req.version==1 and (req.operation=='set_coin' or req.operation=='add_coin')
        if req.operation=='inspect' and req.version==1 then
            if user and user:GetState()>=3 and user:GetCharacNo()>0 then
                receipt.state,receipt.note,receipt.charac_no='done','active_character',user:GetCharacNo()
            end
        elseif not money and not coin then receipt.note='unsupported_operation'
        elseif user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local inventory = user:GetCurCharacInvenR()
            if inventory then
                local get = money and ffi.C.DNF_GM_GetMoney or ffi.C.DNF_GM_GetCoin
                local set = money and ffi.C.DNF_GM_SetMoney or ffi.C.DNF_GM_SetCoin
                local before = tonumber(get(inventory.fptr))
                local after = tonumber(req.amount)
                if req.operation=='add_coin' or req.operation=='add_money' then after = before+after end
                local maximum = money and 2147483647 or 4294967295
                if after>=0 and after<=maximum and after%1==0 and (not money or before<=maximum) then
                    if money and after>before and ffi.C.DNF_GM_MoneyLimit(inventory.fptr,after-before)~=0 then
                        receipt.note='money_limit'
                    else
                        receipt.before,receipt.after = before,after
                        -- From this point a failure is ambiguous: do not label it
                        -- rejected or retry a reward whose memory change may exist.
                        receipt.state,receipt.note = 'unknown','apply_failed'
                        local ok = pcall(function()
                            set(inventory.fptr,after)
                            assert(tonumber(get(inventory.fptr))==after, 'inventory readback differs')
                            if money then assert(user:SendUpdateItemList(0,0), 'money update not sent')
                            else ffi.C.DNF_GM_SendCoin(user.fptr) end
                            assert(ffi.C.DNF_GM_SaveMoneyCoin(user.fptr)==1, 'save not queued')
                        end)
                        if ok then receipt.state,receipt.note = 'applied','save_queued' end
                    end
                else receipt.note = money and 'money_overflow' or 'coin_overflow' end
            else receipt.note = 'inventory_missing' end
        end
        state.receipts[#state.receipts+1] = receipt
    end
end)
state.timer = uv.new_timer()
state.timer:start(100,500,function()
    if state.busy then return end
    config.owner,config.receipts = state.owner,state.receipts
    state.busy = true
    worker:queue(json.encode(config))
end)
_G.DNF_GM_ONLINE = state
_DP.add_unload_handler(function()
    state.stopping = true
    state.timer:stop()
    if not uv.is_closing(state.timer) then state.timer:close() end
end)
io.stderr:write('GM bridge installed; online coin/money commands require an acknowledged save\n')
return state
