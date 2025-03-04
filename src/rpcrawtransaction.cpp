// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "core.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "net.h"
#include "rpcserver.h"
#include "uint256.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#include <stdint.h>
#include <fstream>
#include "hash.h"

#include <openssl/sha.h>
#include <boost/algorithm/string.hpp>
#include <boost/assign/list_of.hpp>
#include "json/json_spirit_utils.h"
#include "json/json_spirit_value.h"

using namespace std;
using namespace boost;
using namespace boost::assign;
using namespace json_spirit;

void ScriptPubKeyToJSON(const CScript& scriptPubKey, Object& out, bool fIncludeHex)
{
    txnouttype type;
    vector<CTxDestination> addresses;
    int nRequired;

    out.push_back(Pair("asm", scriptPubKey.ToString()));
    if (fIncludeHex)
        out.push_back(Pair("hex", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

    if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired))
    {
        out.push_back(Pair("type", GetTxnOutputType(type)));
        return;
    }

    out.push_back(Pair("reqSigs", nRequired));
    out.push_back(Pair("type", GetTxnOutputType(type)));

    Array a;
    BOOST_FOREACH(const CTxDestination& addr, addresses)
        a.push_back(CBitcoinAddress(addr).ToString());
    out.push_back(Pair("addresses", a));
}

void TxToJSON(const CTransaction& tx, const uint256 hashBlock, Object& entry)
{
    entry.push_back(Pair("txid", tx.GetHash().GetHex()));
    entry.push_back(Pair("version", tx.nVersion));
    entry.push_back(Pair("locktime", (int64_t)tx.nLockTime));
    Array vin;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        Object in;
        if (tx.IsCoinBase())
            in.push_back(Pair("coinbase", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        else
        {
            in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
            in.push_back(Pair("vout", (int64_t)txin.prevout.n));
            Object o;
            o.push_back(Pair("asm", txin.scriptSig.ToString()));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            in.push_back(Pair("scriptSig", o));
        }
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    Array vout;
    for (unsigned int i = 0; i < tx.vout.size(); i++)
    {
        const CTxOut& txout = tx.vout[i];
        Object out;
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("n", (int64_t)i));
        Object o;
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        vout.push_back(out);
    }
    entry.push_back(Pair("vout", vout));

    if (hashBlock != 0)
    {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second)
        {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex))
            {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("time", (int64_t)pindex->nTime));
                entry.push_back(Pair("blocktime", (int64_t)pindex->nTime));
            }
            else
                entry.push_back(Pair("confirmations", 0));
        }
    }
}

Value getrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawtransaction \"txid\" ( verbose )\n"
            "\nReturn the raw transaction data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'txid'.\n"
            "If verbose is non-zero, returns an Object with information about 'txid'.\n"

            "\nArguments:\n"
            "1. \"txid\"      (string, required) The transaction id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'txid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",       (string) The serialized, hex-encoded data for 'txid'\n"
            "  \"txid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) \n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n      (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in btc\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"smileycoinaddress\"     (string) smileycoin address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,      (numeric) The confirmations\n"
            "  \"time\" : ttt,             (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"blocktime\" : ttt         (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawtransaction", "\"mytxid\"")
            + HelpExampleCli("getrawtransaction", "\"mytxid\" 1")
            + HelpExampleRpc("getrawtransaction", "\"mytxid\", 1")
        );

    uint256 hash = ParseHashV(params[0], "parameter 1");

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock, true))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;
    string strHex = HexStr(ssTx.begin(), ssTx.end());

    if (!fVerbose)
        return strHex;

    Object result;
    result.push_back(Pair("hex", strHex));
    TxToJSON(tx, hashBlock, result);
    return result;
}

#ifdef ENABLE_WALLET
Value listunspent(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listunspent ( minconf maxconf  [\"address\",...] )\n"
            "\nReturns array of unspent transaction outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "Results are an array of Objects, each of which has:\n"
            "{txid, vout, scriptPubKey, amount, confirmations}\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmationsi to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"    (string) A json array of smileycoin addresses to filter\n"
            "    [\n"
            "      \"address\"   (string) smileycoin address\n"
            "      ,...\n"
            "    ]\n"
            "\nResult\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",        (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",  (string) the smileycoin address\n"
            "    \"account\" : \"account\",  (string) The associated account, or \"\" for the default account\n"
            "    \"scriptPubKey\" : \"key\", (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction amount in btc\n"
            "    \"confirmations\" : n       (numeric) The number of confirmations\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n"
            + HelpExampleCli("listunspent", "")
            + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
        );

    RPCTypeCheck(params, list_of(int_type)(int_type)(array_type));

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    int nMaxDepth = 9999999;
    if (params.size() > 1)
        nMaxDepth = params[1].get_int();

    set<CBitcoinAddress> setAddress;
    if (params.size() > 2)
    {
        Array inputs = params[2].get_array();
        BOOST_FOREACH(Value& input, inputs)
        {
            CBitcoinAddress address(input.get_str());
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Smileycoin address: ")+input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+input.get_str());
           setAddress.insert(address);
        }
    }

    Array results;
    vector<COutput> vecOutputs;
    assert(pwalletMain != NULL);
    pwalletMain->AvailableCoins(vecOutputs, false);
    BOOST_FOREACH(const COutput& out, vecOutputs)
    {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;

        if (setAddress.size())
        {
            CTxDestination address;
            if (!ExtractDestination(out.tx->vout[out.i].scriptPubKey, address))
                continue;

            if (!setAddress.count(address))
                continue;
        }

        int64_t nValue = out.tx->vout[out.i].nValue;
        const CScript& pk = out.tx->vout[out.i].scriptPubKey;
        Object entry;
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));
        CTxDestination address;
        if (ExtractDestination(out.tx->vout[out.i].scriptPubKey, address))
        {
            entry.push_back(Pair("address", CBitcoinAddress(address).ToString()));
            if (pwalletMain->mapAddressBook.count(address))
                entry.push_back(Pair("account", pwalletMain->mapAddressBook[address].name));
        }
        entry.push_back(Pair("scriptPubKey", HexStr(pk.begin(), pk.end())));
        if (pk.IsPayToScriptHash())
        {
            CTxDestination address;
            if (ExtractDestination(pk, address))
            {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwalletMain->GetCScript(hash, redeemScript))
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
            }
        }
        entry.push_back(Pair("amount",ValueFromAmount(nValue)));
        entry.push_back(Pair("confirmations",out.nDepth));
        results.push_back(entry);
    }
    return results;
}
#endif

Value createrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] [{\"address\":amount},{\"data\":\"hex\"},...]\n"
            "\nCreate a transaction spending the given inputs and sending to the given addresses.\n"
            "Returns hex-encoded raw transaction.\n"
            "Note that the transaction's inputs are not signed, and\n"
            "it is not stored in the wallet or transmitted to the network.\n"

            "\nArguments:\n"
            "1. \"transactions\"        (string, required) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"id\",  (string, required) The transaction id\n"
            "         \"vout\":n,       (numeric, required) The output number\n"
            "         \"sequence\":n    (numeric, optional) The sequence number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "2. \"addresses\"           (string, required) a json object with addresses as keys and amounts as values\n"
            "    {\n"
            "      \"address\": x.xxx   (numeric, required) The key is the smileycoin address, the value is the btc amount\n"
            "      ,...\n"
            "    },\n"
            "    {\n"
            "      \"data\": \"hex:amount\"    (obj, optional) A key-value pair. The key must be \"data\", the value is hex encoded data and amount of SMLY\n"
            "                                  to be burnt by this OP_RETURN output. Default value is 0\n"
            "    }\n"
            "    ,...                   More key-value pairs of the above form. For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
            "                           accepted as second parameter.\n"
            "\nResult:\n"
            "\"transaction\"            (string) hex string of the transaction\n"

            "\nExamples\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"address\\\":0.01}]\"")
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"[{\\\"address\\\":0.01}]\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
        );

    RPCTypeCheck(params, list_of(array_type)(obj_type));

    Array inputs = params[0].get_array();
    Object sendTo = params[1].get_obj();

    CMutableTransaction rawTx;

    BOOST_FOREACH(const Value& input, inputs)
    {
        const Object& o = input.get_obj();

        uint256 txid = ParseHashO(o, "txid");

        const Value& vout_v = find_value(o, "vout");
        if (vout_v.type() != int_type)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        CTxIn in(COutPoint(txid, nOutput));

        const Value& sequence = find_value(o, "sequence");
        if (sequence.type() == int_type) {
            int nSequence = sequence.get_int();
            if (nSequence < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, sequence must be positive");
            in.nSequence = nSequence;
        }

        rawTx.vin.push_back(in);
    }

    set<CBitcoinAddress> setAddress;

    int64_t DEFAULT_AMOUNT = 0;

    BOOST_FOREACH(const Pair& s, sendTo)
    {
        if(s.name_ == "data") {
            vector<string> str;
            string hexdata = s.value_.get_str();
            int64_t amount = 0;

            split(str,hexdata,boost::is_any_of(":"));
            vector<unsigned char> data = ParseHexV(str[0], "Data");

            if(str.size() == 2) {
                amount = strtoll(str[1].c_str(),NULL,10);
            }

            CTxOut out(max(DEFAULT_AMOUNT,amount) * COIN, CScript() << OP_RETURN << data);
            rawTx.vout.push_back(out);
        } else {
            CBitcoinAddress address(s.name_);
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Smileycoin address: ")+s.name_);

            if (setAddress.count(address))
                throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+s.name_);
            setAddress.insert(address);

            CScript scriptPubKey;
            scriptPubKey.SetDestination(address.Get());
            int64_t nAmount = AmountFromValue(s.value_);

            CTxOut out(nAmount, scriptPubKey);
            rawTx.vout.push_back(out);
        }
    }

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << rawTx;
    return HexStr(ss.begin(), ss.end());
}

Value decoderawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawtransaction \"hexstring\"\n"
            "\nReturn a JSON object representing the serialized, hex-encoded transaction.\n"

            "\nArguments:\n"
            "1. \"hex\"      (string, required) The transaction hex string\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"id\",        (string) The transaction id\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) The output number\n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n     (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [             (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in btc\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"12tvKAXCxZjSmdNbao16dKXC8tRWfcF5oc\"   (string) smileycoin address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawtransaction", "\"hexstring\"")
            + HelpExampleRpc("decoderawtransaction", "\"hexstring\"")
        );

    vector<unsigned char> txData(ParseHexV(params[0], "argument"));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    try {
        ssData >> tx;
    }
    catch (std::exception &e) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    Object result;
    TxToJSON(tx, 0, result);

    return result;
}

Value decodescript(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decodescript \"hex\"\n"
            "\nDecode a hex-encoded script.\n"
            "\nArguments:\n"
            "1. \"hex\"     (string) the hex encoded script\n"
            "\nResult:\n"
            "{\n"
            "  \"asm\":\"asm\",   (string) Script public key\n"
            "  \"hex\":\"hex\",   (string) hex encoded public key\n"
            "  \"type\":\"type\", (string) The output type\n"
            "  \"reqSigs\": n,    (numeric) The required signatures\n"
            "  \"addresses\": [   (json array of string)\n"
            "     \"address\"     (string) smileycoin address\n"
            "     ,...\n"
            "  ],\n"
            "  \"p2sh\",\"address\" (string) script address\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("decodescript", "\"hexstring\"")
            + HelpExampleRpc("decodescript", "\"hexstring\"")
        );

    RPCTypeCheck(params, list_of(str_type));

    Object r;
    CScript script;
    if (params[0].get_str().size() > 0){
        vector<unsigned char> scriptData(ParseHexV(params[0], "argument"));
        script = CScript(scriptData.begin(), scriptData.end());
    } else {
        // Empty scripts are valid
    }
    ScriptPubKeyToJSON(script, r, false);

    r.push_back(Pair("p2sh", CBitcoinAddress(script.GetID()).ToString()));
    return r;
}

Value signrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 4)
        throw runtime_error(
            "signrawtransaction \"hexstring\" ( [{\"txid\":\"id\",\"vout\":n,\"scriptPubKey\":\"hex\",\"redeemScript\":\"hex\"},...] [\"privatekey1\",...] sighashtype )\n"
            "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of previous transaction outputs that\n"
            "this transaction depends on but may not yet be in the block chain.\n"
            "The third optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"prevtxs\"       (string, optional) An json array of previous dependent transaction outputs\n"
            "     [               (json array of json objects, or 'null' if none provided)\n"
            "       {\n"
            "         \"txid\":\"id\",             (string, required) The transaction id\n"
            "         \"vout\":n,                  (numeric, required) The output number\n"
            "         \"scriptPubKey\": \"hex\",   (string, required) script key\n"
            "         \"redeemScript\": \"hex\"    (string, required) redeem script\n"
            "       }\n"
            "       ,...\n"
            "    ]\n"
            "3. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"
            "4. \"sighashtype\"     (string, optional, default=ALL) The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\"\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\": \"value\",   (string) The raw transaction with signature(s) (hex-encoded string)\n"
            "  \"complete\": n       (numeric) if transaction has a complete set of signature (0 if not)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

    RPCTypeCheck(params, list_of(str_type)(array_type)(array_type)(str_type), true);

    vector<unsigned char> txData(ParseHexV(params[0], "argument 1"));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    vector<CMutableTransaction> txVariants;
    while (!ssData.empty())
    {
        try {
            CMutableTransaction tx;
            ssData >> tx;
            txVariants.push_back(tx);
        }
        catch (std::exception &e) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
    }

    if (txVariants.empty())
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing transaction");

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the rawtx:
    CMutableTransaction mergedTx(txVariants[0]);
    bool fComplete = true;

    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedTx.vin) {
            const uint256& prevHash = txin.prevout.hash;
            CCoins coins;
            view.GetCoins(prevHash, coins); // this is certainly allowed to fail
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 2 && params[2].type() != null_type)
    {
        fGivenKeys = true;
        Array keys = params[2].get_array();
        BOOST_FOREACH(Value k, keys)
        {
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey();
            tempKeystore.AddKey(key);
        }
    }
#ifdef ENABLE_WALLET
    else
        EnsureWalletIsUnlocked();
#endif

    // Add previous txouts given in the RPC call:
    if (params.size() > 1 && params[1].type() != null_type)
    {
        Array prevTxs = params[1].get_array();
        BOOST_FOREACH(Value& p, prevTxs)
        {
            if (p.type() != obj_type)
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");

            Object prevOut = p.get_obj();

            RPCTypeCheck(prevOut, map_list_of("txid", str_type)("vout", int_type)("scriptPubKey", str_type));

            uint256 txid = ParseHashO(prevOut, "txid");

            int nOut = find_value(prevOut, "vout").get_int();
            if (nOut < 0)
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout must be positive");

            vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            CCoins coins;
            if (view.GetCoins(txid, coins)) {
                if (coins.IsAvailable(nOut) && coins.vout[nOut].scriptPubKey != scriptPubKey) {
                    string err("Previous output scriptPubKey mismatch:\n");
                    err = err + coins.vout[nOut].scriptPubKey.ToString() + "\nvs:\n"+
                        scriptPubKey.ToString();
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                // what todo if txid is known, but the actual output isn't?
            }
            if ((unsigned int)nOut >= coins.vout.size())
                coins.vout.resize(nOut+1);
            coins.vout[nOut].scriptPubKey = scriptPubKey;
            coins.vout[nOut].nValue = 0; // we don't know the actual output value
            view.SetCoins(txid, coins);

            // if redeemScript given and not using the local wallet (private keys
            // given), add redeemScript to the tempKeystore so it can be signed:
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash())
            {
                RPCTypeCheck(prevOut, map_list_of("txid", str_type)("vout", int_type)("scriptPubKey", str_type)("redeemScript",str_type));
                Value v = find_value(prevOut, "redeemScript");
                if (!(v == Value::null))
                {
                    vector<unsigned char> rsData(ParseHexV(v, "redeemScript"));
                    CScript redeemScript(rsData.begin(), rsData.end());
                    tempKeystore.AddCScript(redeemScript);
                }
            }
        }
    }

#ifdef ENABLE_WALLET
    const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
#else
    const CKeyStore& keystore = tempKeystore;
#endif

    int nHashType = SIGHASH_ALL;
    if (params.size() > 3 && params[3].type() != null_type)
    {
        static map<string, int> mapSigHashValues =
            boost::assign::map_list_of
            (string("ALL"), int(SIGHASH_ALL))
            (string("ALL|ANYONECANPAY"), int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))
            (string("NONE"), int(SIGHASH_NONE))
            (string("NONE|ANYONECANPAY"), int(SIGHASH_NONE|SIGHASH_ANYONECANPAY))
            (string("SINGLE"), int(SIGHASH_SINGLE))
            (string("SINGLE|ANYONECANPAY"), int(SIGHASH_SINGLE|SIGHASH_ANYONECANPAY))
            ;
        string strHashType = params[3].get_str();
        if (mapSigHashValues.count(strHashType))
            nHashType = mapSigHashValues[strHashType];
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash param");
    }

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++)
    {
        CTxIn& txin = mergedTx.vin[i];
        CCoins coins;
        if (!view.GetCoins(txin.prevout.hash, coins) || !coins.IsAvailable(txin.prevout.n))
        {
            fComplete = false;
            continue;
        }
        const CScript& prevPubKey = coins.vout[txin.prevout.n].scriptPubKey;

        txin.scriptSig.clear();
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.vout.size()))
            SignSignature(keystore, prevPubKey, mergedTx, i, nHashType);

        // ... and merge in other signatures:
        BOOST_FOREACH(const CMutableTransaction& txv, txVariants)
        {
            txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig);
        }
        if (!VerifyScript(txin.scriptSig, prevPubKey, mergedTx, i, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, 0))
            fComplete = false;
    }

    Object result;
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << mergedTx;
    result.push_back(Pair("hex", HexStr(ssTx.begin(), ssTx.end())));
    result.push_back(Pair("complete", fComplete));

    return result;
}

Value sendrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw transaction (serialized, hex-encoded) to local node and network.\n"
            "\nAlso see createrawtransaction and signrawtransaction calls.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"{\\\"myaddress\\\":0.01}\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"") +
            "\nSend the transaction (signed hex)\n"
            + HelpExampleCli("sendrawtransaction", "\"signedhex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawtransaction", "\"signedhex\"")
        );


    // parse hex string from parameter
    vector<unsigned char> txData(ParseHexV(params[0], "parameter"));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;

    bool fOverrideFees = false;
    if (params.size() > 1)
        fOverrideFees = params[1].get_bool();

    // deserialize binary data stream
    try {
        ssData >> tx;
    }
    catch (std::exception &e) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }
    uint256 hashTx = tx.GetHash();

    CCoinsViewCache &view = *pcoinsTip;
    CCoins existingCoins;
    bool fHaveMempool = mempool.exists(hashTx);
    bool fHaveChain = view.GetCoins(hashTx, existingCoins) && existingCoins.nHeight < 1000000000;
    if (!fHaveMempool && !fHaveChain) {
        // push to local node and sync with wallets
        CValidationState state;
        if (AcceptToMemoryPool(mempool, state, tx, false, NULL, !fOverrideFees))
            SyncWithWallets(hashTx, tx, NULL);
        else {
            if(state.IsInvalid())
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", state.GetRejectCode(), state.GetRejectReason()));
            else
                throw JSONRPCError(RPC_TRANSACTION_ERROR, state.GetRejectReason());
        }
    } else if (fHaveChain) {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "transaction already in block chain");
    }
    RelayTransaction(tx, hashTx);

    return hashTx.GetHex();
}

Value lottery(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error(
            "lottery \"amount\"\nSend an amount to the lottery. The amount must be greater than 1000"
            " to get in the lottery."
            "\nArguments:\n"
            "1. \"amount\"       (numeric, required) The amount in smly to send to the lottery\n"
            "Result:\n"
            "\"transactionid\" (string) The transaction id.\n"
            "\nExamples\n"
            + HelpExampleCli("lottery", "1000")
        );

    //The lottery address
    CBitcoinAddress address("BE8svSuyAuFFm1RFC8CGWXxyHCKjKBEYQW");

    int64_t nAmount = AmountFromValue(params[0]);

    CWalletTx wtx;

    EnsureWalletIsUnlocked();

    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value signrawtokentransaction(string txDataHex) {
  vector<unsigned char> txData(ParseHexV(txDataHex, "argument 1"));
  CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
  vector<CMutableTransaction> txVariants;
  while (!ssData.empty())
  {
      try {
          CMutableTransaction tx;
          ssData >> tx;
          txVariants.push_back(tx);
      }
      catch (std::exception &e) {
          throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
      }
  }

  if (txVariants.empty())
      throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing transaction");

  // mergedTx will end up with all the signatures; it
  // starts as a clone of the rawtx:
  CMutableTransaction mergedTx(txVariants[0]);
  bool fComplete = true;

  // Fetch previous transactions (inputs):
  CCoinsView viewDummy;
  CCoinsViewCache view(viewDummy);
  {
      LOCK(mempool.cs);
      CCoinsViewCache &viewChain = *pcoinsTip;
      CCoinsViewMemPool viewMempool(viewChain, mempool);
      view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

      BOOST_FOREACH(const CTxIn& txin, mergedTx.vin) {
          const uint256& prevHash = txin.prevout.hash;
          CCoins coins;
          view.GetCoins(prevHash, coins); // this is certainly allowed to fail
      }

      view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
  }

  bool fGivenKeys = false;
  CBasicKeyStore tempKeystore;

  #ifdef ENABLE_WALLET
    EnsureWalletIsUnlocked();
  #endif

  #ifdef ENABLE_WALLET
  const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
  #else
  const CKeyStore& keystore = tempKeystore;
  #endif

  int nHashType = SIGHASH_ALL;

  bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

  // Sign what we can:
  for (unsigned int i = 0; i < mergedTx.vin.size(); i++)
  {
      CTxIn& txin = mergedTx.vin[i];
      CCoins coins;
      if (!view.GetCoins(txin.prevout.hash, coins) || !coins.IsAvailable(txin.prevout.n))
      {
          fComplete = false;
          continue;
      }
      const CScript& prevPubKey = coins.vout[txin.prevout.n].scriptPubKey;

      txin.scriptSig.clear();
      // Only sign SIGHASH_SINGLE if there's a corresponding output:
      if (!fHashSingle || (i < mergedTx.vout.size()))
          SignSignature(keystore, prevPubKey, mergedTx, i, nHashType);

      // ... and merge in other signatures:
      BOOST_FOREACH(const CMutableTransaction& txv, txVariants)
      {
          txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig);
      }
      if (!VerifyScript(txin.scriptSig, prevPubKey, mergedTx, i, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, 0))
          fComplete = false;
  }
  CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
  ssTx << mergedTx;

  return HexStr(ssTx.begin(), ssTx.end());
}

string GetHexStringFromBytes(vector<unsigned char> c) {
  string s = "";

  for(char character : c) {
    int firstHalf = (character >> 4) & (unsigned char)(0x0F);
    int secondHalf = character & (unsigned char)(0x0F);

    if(firstHalf == 0) {
      s = s + "0";
    } else if(firstHalf == 1) {
      s = s + "1";
    } else if(firstHalf == 2) {
      s = s + "2";
    } else if(firstHalf == 3) {
      s = s + "3";
    } else if(firstHalf == 4) {
      s = s + "4";
    } else if(firstHalf == 5) {
      s = s + "5";
    } else if(firstHalf == 6) {
      s = s + "6";
    } else if(firstHalf == 7) {
      s = s + "7";
    } else if(firstHalf == 8) {
      s = s + "8";
    } else if(firstHalf == 9) {
      s = s + "9";
    } else if(firstHalf == 10) {
      s = s + "a";
    } else if(firstHalf == 11) {
      s = s + "b";
    } else if(firstHalf == 12) {
      s = s + "c";
    } else if(firstHalf == 13) {
      s = s + "d";
    } else if(firstHalf == 14) {
      s = s + "e";
    } else if(firstHalf == 15) {
      s = s + "f";
    }

    if(secondHalf == 0) {
      s = s + "0";
    } else if(secondHalf == 1) {
      s = s + "1";
    } else if(secondHalf == 2) {
      s = s + "2";
    } else if(secondHalf == 3) {
      s = s + "3";
    } else if(secondHalf == 4) {
      s = s + "4";
    } else if(secondHalf == 5) {
      s = s + "5";
    } else if(secondHalf == 6) {
      s = s + "6";
    } else if(secondHalf == 7) {
      s = s + "7";
    } else if(secondHalf == 8) {
      s = s + "8";
    } else if(secondHalf == 9) {
      s = s + "9";
    } else if(secondHalf == 10) {
      s = s + "a";
    } else if(secondHalf == 11) {
      s = s + "b";
    } else if(secondHalf == 12) {
      s = s + "c";
    } else if(secondHalf == 13) {
      s = s + "d";
    } else if(secondHalf == 14) {
      s = s + "e";
    } else if(secondHalf == 15) {
      s = s + "f";
    }
  }

  return s;
}

Value createtoken(const Array& params, bool fhelp) {
  if(fhelp || params.size() < 2 || params.size() > 2) {
    throw runtime_error(
        "createtoken \"absolute_path_to_file\" \"previous_tx_id\" "
        "\ncreate a token from file specified and an output from the transactions with id: transaction_id needs to be used in creation"
        "\nArguments:\n"
        "1. \"absolute_path_to_file\"       (string, required) Absolute path to file to be tokenized\n"
        "2. \"previous_tx_id\" (string, required) In order for the token to be valid output from this transaction needs to be used for creating the token"
        "Result:\n"
        "\"tokenid\" (string) The token id.\n"
        "\nExamples\n"
        + HelpExampleCli("lottery", "1000")
    );
  }

  //Opna skranna
  ifstream file(params[0].get_str(), ifstream::binary);
  file.seekg(0, file.end);
  int length = file.tellg();
  file.seekg(0, file.beg);

  char buffer[length];

  file.read(buffer, length);
  file.close();

  string filestring(buffer);

  //Bæti transaction id á færslunni á undan:
  filestring = filestring + params[1].get_str();

  CHashWriter hashWriter(1, 1);

  hashWriter.write((const char*) &filestring[0], 64);

  uint256 hash = hashWriter.GetHash();

  CKey key;
  key.MakeNewKey(false);
  CPubKey pubKey = key.GetPubKey();

  vector<unsigned char> signature;

  key.Sign(hash, signature);

  string pubKeyString = pubKey.GetHash().GetHex();
  string signatureHexString = GetHexStringFromBytes(signature);
  string privateKeyHex = CBitcoinSecret(key).ToString();

  Object result;

  result.push_back(Pair("Token ID", signatureHexString));
  result.push_back(Pair("Token public key", pubKeyString));
  result.push_back(Pair("Token private key", privateKeyHex));

  return result;
}

//Nota í inittoken aðferðinni.
Value CreateToken(string pathToFile, string previousTxId) {

  //Opna skranna
  ifstream file(pathToFile, ifstream::binary);
  file.seekg(0, file.end);
  int length = file.tellg();
  file.seekg(0, file.beg);

  char buffer[length];

  file.read(buffer, length);
  file.close();

  string filestring(buffer);

  //Bæti transaction id á færslunni á undan:
  filestring = filestring + previousTxId;

  CHashWriter hashWriter(1, 1);

  hashWriter.write((const char*) &filestring[0], 64);

  uint256 hash = hashWriter.GetHash();

  CKey key;
  key.MakeNewKey(false);
  CPubKey pubKey = key.GetPubKey();

  vector<unsigned char> signature;

  key.Sign(hash, signature);

  string pubKeyString = pubKey.GetHash().GetHex();
  string signatureHexString = GetHexStringFromBytes(signature);
  string privateKeyHex = CBitcoinSecret(key).ToString();

  Object result;

  result.push_back(Pair("Token ID", signatureHexString));
  result.push_back(Pair("Token public key", pubKeyString));
  result.push_back(Pair("Token private key", privateKeyHex));

  return result;

}

//fall sem sendir 1 á address sem veskið á og skilar outputinu og n.
uint256 makeInputTransactionForToken(int am) {

  Value amount(am);
  CWalletTx wtx;

  const CBitcoinAddress& walletAddress = (pwalletMain->mapAddressBook.begin())->first;
  //Fer fyrst í gegnum addresses sem eru til í þessu veski.
  /*BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, CAddressBookData)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& walletAddress = item.first;
    break;
  }*/

  //Notum aðferð tvö svo að við notum ekki transactions með op_return svæði sem input.
  string strError = pwalletMain->SendMoneyToDestination2(walletAddress.Get(), AmountFromValue(amount), wtx);
  if(strError != "") {
    throw JSONRPCError(RPC_WALLET_ERROR, strError);
  }

  return wtx.GetHash();
}

/*Value sendtokentoaddress(const Array& params, bool fhelp) {
  if(fhelp || params.size() < 2 || params.size() > 2) {
    throw runtime_error(
        "sendtokentoaddress \"txid\" \"address\" \"n\""
        "\n send a token that's stored in transaction with id \"txid\" to address \"address\""
        "\nArguments:\n"
        "1. \"txid\"       (string, required) tx where token is stored\n"
        "2. \"smlyaddress\" smileyaddress of where the token should be sent to\n"
        "Result:\n"
        "\"complete\" (bool) true if successful false otherwise.\n"
        "\nExamples\n"
        + HelpExampleCli("lottery", "1000")
      );
  }

  uint256 inputtx = makeInputTransactionForToken();
  int nOutput = 1

  uint256 previousTokenTx = ParseHexV(params[0], "Parameter 1");

  CMutableTransaction rawtx;

  CTxIn in(COutPoint(inputtxid, nOutput));
  rawtx.

}*/


Value inittoken(const Array& params, bool fhelp) {

  if(fhelp || params.size() < 2 || params.size() > 2) {
    throw runtime_error(
        "inittoken \"smlyaddress\" \"pathToFile\" \"n\""
        "\ncreate a token from file specified and an output from the transactions with id: transaction_id needs to be used in creation"
        "\nArguments:\n"
        "1. \"smlyaddress\" smileyaddress of where the token should be sent to"
        "2. \"PathToFile\" the path to the file that is to be reperesented as a token"
        "Result:\n"
        "\"tokenid\", \"token private key\", \"token public key\", \"transaction id\"\n"
      );
  }

  //1 SMly fer í færslugjöld
  uint256 inputtxid = makeInputTransactionForToken(1001);

  //skoða inputfærsluna og finn rétta outputið.
  CTransaction inputTransaction;
  uint256 hashBlock;
  if(!GetTransaction(inputtxid, inputTransaction, hashBlock, false))
    throw runtime_error("Error");

  //Athugum hvar output 1001 sé.
  int nOutput = 0;
  int64_t checkAmount = 1001*COIN;
  BOOST_FOREACH(const CTxOut& inputTransactionOut, inputTransaction.vout) {
    if(inputTransactionOut.nValue == checkAmount)
      break;

    nOutput = nOutput + 1;
  }

  //Token er búinn til með inputtxid.
  Value token = CreateToken(params[1].get_str(), inputtxid.GetHex());

  CMutableTransaction rawTx;

  //notum bara þetta sem input.
  CTxIn in(COutPoint(inputtxid, nOutput));

  rawTx.vin.push_back(in);

  CBitcoinAddress address(params[0].get_str());

  if(!address.IsValid()) {
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid smly address"));
  }

  CScript scriptPubKey;
  scriptPubKey.SetDestination(address.Get());

  CTxOut out(1000 * COIN, scriptPubKey);

  rawTx.vout.push_back(out);

  //string hexdata = params[0].get_str();
  //vector<unsigned char> data = ParseHexV(params[0], "data");
  vector<unsigned char> data = ParseHexO(token.get_obj(), "Token ID");

  CTxOut out2(0, CScript() << OP_RETURN << data);
  rawTx.vout.push_back(out2);

  CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
  ss << rawTx;

  Value signedValue = signrawtokentransaction(HexStr(ss.begin(), ss.end()));

  // parse hex string from parameter
  vector<unsigned char> txData(ParseHexV(signedValue, "parameter"));
  CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
  CTransaction tx;

  bool fOverrideFees = false;

  // deserialize binary data stream
  try {
      ssData >> tx;
  }
  catch (std::exception &e) {
      throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
  }

  uint256 hashTx = tx.GetHash();
  CCoinsViewCache &view = *pcoinsTip;
  CCoins existingCoins;
  bool fHaveMempool = mempool.exists(hashTx);
  bool fHaveChain = view.GetCoins(hashTx, existingCoins) && existingCoins.nHeight < 1000000000;
  if (!fHaveMempool && !fHaveChain) {
      // push to local node and sync with wallets
      CValidationState state;
      if (AcceptToMemoryPool(mempool, state, tx, false, NULL, !fOverrideFees))
          SyncWithWallets(hashTx, tx, NULL);
      else {
          if(state.IsInvalid())
              throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", state.GetRejectCode(), state.GetRejectReason()));
          else
              throw JSONRPCError(RPC_TRANSACTION_ERROR, state.GetRejectReason());
      }
  } else if (fHaveChain) {
      throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "transaction already in block chain");
  }

  RelayTransaction(tx, hashTx);

  Object result;
  Object tokenObject = token.get_obj();

  result.push_back(Pair("Token ID", tokenObject[0].value_.get_str()));
  result.push_back(Pair("Token public key", tokenObject[1].value_.get_str()));
  result.push_back(Pair("Token private key", tokenObject[2].value_.get_str()));
  result.push_back(Pair("transactionid", hashTx.GetHex()));

  return result;
}
