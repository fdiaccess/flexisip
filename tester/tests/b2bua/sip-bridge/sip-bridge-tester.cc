/** Copyright (C) 2010-2024 Belledonne Communications SARL
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "b2bua/sip-bridge/sip-bridge.hh"

#include <chrono>

#include "soci/session.h"
#include "soci/sqlite3/soci-sqlite3.h"

#include "belle-sip/auth-helper.h"

#include "b2bua/b2bua-server.hh"
#include "registrardb-internal.hh"
#include "utils/client-builder.hh"
#include "utils/client-call.hh"
#include "utils/client-core.hh"
#include "utils/core-assert.hh"
#include "utils/proxy-server.hh"
#include "utils/redis-server.hh"
#include "utils/string-formatter.hh"
#include "utils/temp-file.hh"
#include "utils/test-patterns/test.hh"
#include "utils/test-suite.hh"
#include "utils/tmp-dir.hh"

namespace flexisip::tester {
namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;

/*
    Test bridging to *and* from an external sip provider/domain. (Arbitrarily called "Jabiru")
    We configure 2 providers, one for each direction.

    The first, "Outbound" provider will attempt to find an external account matching the caller, and bridge the call
    using that account.
    The second, "Inbound" provider will attempt to find the external account that received the call to determine the uri
    to call in the internal domain, and send the invite to the flexisip proxy.

    We'll need a user registered to the internal Flexisip proxy. Let's call him Felix <sip:felix@flexisip.example.org>.
    Felix will need an account on the external Jabiru proxy, with a potentially different username than the one he uses
    on Flexisip: <sip:definitely-not-felix@jabiru.example.org>. That account will be provisioned in the B2BUA's account
    pool.
    Then we'll need a user registered to the Jabiru proxy, let's call him Jasper <sip:jasper@jabiru.example.org>.

    Felix will first attempt to call Jasper as if he was in the same domain as him, using the address
    <sip:jasper@flexisip.example.org>. Jasper should receive a bridged call coming from
    <sip:definitely-not-felix@jabiru.example.org>, Felix's external account managed by the B2BUA.

    Then Jasper will in turn attempt to call Felix's external account, <sip:definitely-not-felix@jabiru.example.org>,
    and Felix should receive a call form Jasper that should look like it's coming from within the same domain as him:
    <sip:jasper@flexisip.example.org>
*/

/**
 * TODO: There should really be 2 different proxies, to test that the Inbound provider can correctly send invites to the
 * Flexisip proxy and not the `outboundProxy` configured on the B2BUA account. If done, use
 * Module::Router/static-targets instead of module::B2Bua/b2bua-server.
 */
void bidirectionalBridging() {
	StringFormatter jsonConfig{R"json({
		"schemaVersion": 2,
		"providers": [
			{
				"name": "Flexisip -> Jabiru (Outbound)",
				"triggerCondition": {
					"strategy": "Always"
				},
				"accountToUse": {
					"strategy": "FindInPool",
					"source": "{from}",
					"by": "alias"
				},
				"onAccountNotFound": "nextProvider",
				"outgoingInvite": {
					"to": "sip:{incoming.to.user}@{account.uri.hostport}{incoming.to.uriParameters}",
					"from": "{account.uri}"
				},
				"accountPool": "FlockOfJabirus"
			},
			{
				"name": "Jabiru -> Flexisip (Inbound)",
				"triggerCondition": {
					"strategy": "Always"
				},
				"accountToUse": {
					"strategy": "FindInPool",
					"source": "{to}",
					"by": "uri"
				},
				"onAccountNotFound": "nextProvider",
				"outgoingInvite": {
					"to": "{account.alias}",
					"from": "sip:{incoming.from.user}@{account.alias.hostport}{incoming.from.uriParameters}",
					"outboundProxy": "<sip:127.0.0.1:port;transport=tcp>"
				},
				"accountPool": "FlockOfJabirus"
			}
		],
		"accountPools": {
			"FlockOfJabirus": {
				"outboundProxy": "<sip:127.0.0.1:port;transport=tcp>",
				"registrationRequired": true,
				"maxCallsPerLine": 3125,
				"loader": [
					{
						"uri": "sip:definitely-not-felix@jabiru.example.org",
						"alias": "sip:felix@flexisip.example.org"
					}
				]
			}
		}
	})json",
	                           '', ''};
	TempFile providersJson{};
	Server proxy{{
	    // Requesting bind on port 0 to let the kernel find any available port
	    {"global/transports", "sip:127.0.0.1:0;transport=tcp"},
	    {"module::Registrar/enabled", "true"},
	    {"module::Registrar/reg-domains", "flexisip.example.org jabiru.example.org"},
	    {"b2bua-server/application", "sip-bridge"},
	    {"b2bua-server/transport", "sip:127.0.0.1:0;transport=tcp"},
	    {"b2bua-server::sip-bridge/providers", providersJson.getFilename()},
	    {"module::B2bua/enabled", "true"},
	}};
	proxy.start();
	providersJson.writeStream() << jsonConfig.format({{"port", proxy.getFirstPort()}});
	const auto b2buaLoop = std::make_shared<sofiasip::SuRoot>();
	const auto& config = proxy.getConfigManager();
	const auto b2buaServer = std::make_shared<B2buaServer>(b2buaLoop, config);
	b2buaServer->init();
	config->getRoot()
	    ->get<GenericStruct>("module::B2bua")
	    ->get<ConfigString>("b2bua-server")
	    ->set("sip:127.0.0.1:" + std::to_string(b2buaServer->getTcpPort()) + ";transport=tcp");
	proxy.getAgent()->findModule("B2bua")->reload();
	auto builder = ClientBuilder(*proxy.getAgent());
	const auto felix = builder.build("felix@flexisip.example.org");
	const auto jasper = builder.build("jasper@jabiru.example.org");
	CoreAssert asserter{proxy, *b2buaLoop};
	asserter
	    .iterateUpTo(
	        3,
	        [&sipProviders =
	             dynamic_cast<const b2bua::bridge::SipBridge&>(b2buaServer->getApplication()).getProviders()] {
		        for (const auto& provider : sipProviders) {
			        for (const auto& [_, account] : provider.getAccountSelectionStrategy().getAccountPool()) {
				        FAIL_IF(!account->isAvailable());
			        }
		        }
		        // b2bua accounts registered
		        return ASSERTION_PASSED();
	        },
	        40ms)
	    .assert_passed();
	asserter.registerSteppable(felix);
	asserter.registerSteppable(jasper);

	// Flexisip -> Jabiru
	felix.invite("jasper@flexisip.example.org");
	BC_HARD_ASSERT(asserter
	                   .iterateUpTo(
	                       3,
	                       [&jasper] {
		                       const auto& current_call = jasper.getCurrentCall();
		                       FAIL_IF(current_call == std::nullopt);
		                       FAIL_IF(current_call->getState() != linphone::Call::State::IncomingReceived);
		                       // invite received
		                       return ASSERTION_PASSED();
	                       },
	                       300ms)
	                   .assert_passed());
	BC_ASSERT_CPP_EQUAL(jasper.getCurrentCall()->getRemoteAddress()->asStringUriOnly(),
	                    "sip:definitely-not-felix@jabiru.example.org");

	// cleanup
	jasper.getCurrentCall()->accept();
	jasper.getCurrentCall()->terminate();
	asserter
	    .iterateUpTo(
	        2,
	        [&felix] {
		        const auto& current_call = felix.getCurrentCall();
		        FAIL_IF(current_call != std::nullopt);
		        return ASSERTION_PASSED();
	        },
	        90ms)
	    .assert_passed();

	// Jabiru -> Flexisip
	jasper.invite("definitely-not-felix@jabiru.example.org");
	BC_HARD_ASSERT(asserter
	                   .iterateUpTo(
	                       2,
	                       [&felix] {
		                       const auto& current_call = felix.getCurrentCall();
		                       FAIL_IF(current_call == std::nullopt);
		                       FAIL_IF(current_call->getState() != linphone::Call::State::IncomingReceived);
		                       // invite received
		                       return ASSERTION_PASSED();
	                       },
	                       400ms)
	                   .assert_passed());
	BC_ASSERT_CPP_EQUAL(felix.getCurrentCall()->getRemoteAddress()->asStringUriOnly(),
	                    "sip:jasper@flexisip.example.org");

	// shutdown / cleanup
	felix.getCurrentCall()->accept();
	felix.getCurrentCall()->terminate();
	asserter
	    .iterateUpTo(
	        2,
	        [&jasper] {
		        const auto& current_call = jasper.getCurrentCall();
		        FAIL_IF(current_call != std::nullopt);
		        return ASSERTION_PASSED();
	        },
	        400ms)
	    .assert_passed();
	std::ignore = b2buaServer->stop();
}

void loadAccountsFromSQL() {
	TmpDir sqliteDbDir{"b2bua::bridge::loadAccountsFromSQL"};
	const auto& sqliteDbFilePath = sqliteDbDir.path() / "db.sqlite";
	const auto& providersConfigPath = sqliteDbDir.path() / "providers.json";
	try {
		soci::session sql(soci::sqlite3, sqliteDbFilePath);
		sql << R"sql(CREATE TABLE users (
						username TEXT PRIMARY KEY,
						hostport TEXT,
						userid TEXT,
						passwordInDb TEXT,
						alias_username TEXT,
						alias_hostport TEXT,
						outboundProxyInDb TEXT))sql";
		sql << R"sql(INSERT INTO users VALUES ("account1", "some.provider.example.com", "", "", "alias", "sip.example.org", ""))sql";
		sql << R"sql(INSERT INTO users VALUES ("account2", "some.provider.example.com", "test-userID", "clear text passphrase", "", "", "sip.linphone.org"))sql";
		sql << R"sql(INSERT INTO users VALUES ("account3", "some.provider.example.com", "", "", "", "", ""))sql";
	} catch (const soci::soci_error& e) {
		auto msg = "Error initiating DB : "s + e.what();
		BC_HARD_FAIL(msg.c_str());
	}
	StringFormatter jsonConfig{R"json({
		"schemaVersion": 2,
		"providers": [
			{
				"name": "Stub Provider",
				"triggerCondition": { "strategy": "Always" },
				"accountToUse": { "strategy": "Random" },
				"onAccountNotFound": "decline",
				"outgoingInvite": { "to": "{incoming.to}" },
				"accountPool": "FlockOfJabirus"
			}
		],
		"accountPools": {
			"FlockOfJabirus": {
				"outboundProxy": "<sip:127.0.0.1:port;transport=tcp>",
				"registrationRequired": true,
				"maxCallsPerLine": 3125,
				"loader": {
					"dbBackend": "sqlite3",
					"initQuery": "SELECT username, hostport, userid as user_id, \"clrtxt\" as secret_type, passwordInDb as secret, alias_username, alias_hostport, outboundProxyInDb as outbound_proxy from users",
					"updateQuery": "not yet implemented",
					"connection": "db-file-path"
				}
			}
		}
	})json",
	                           '', ''};
	auto redis = RedisServer();
	Server proxy{{
	    // Requesting bind on port 0 to let the kernel find any available port
	    {"global/transports", "sip:127.0.0.1:0;transport=tcp"},
	    {"module::Registrar/enabled", "true"},
	    {"module::Registrar/reg-domains", "some.provider.example.com"},
	    {"module::Registrar/db-implementation", "redis"},
	    {"module::Registrar/redis-server-domain", "localhost"},
	    {"module::Registrar/redis-server-port", std::to_string(redis.port())},
	    {"b2bua-server/application", "sip-bridge"},
	    {"b2bua-server/transport", "sip:127.0.0.1:0;transport=tcp"},
	    {"b2bua-server::sip-bridge/providers", providersConfigPath},
	}};
	proxy.start();
	std::ofstream{providersConfigPath} << jsonConfig.format({
	    {"port", proxy.getFirstPort()},
	    {"db-file-path", sqliteDbFilePath},
	});
	const auto b2buaLoop = std::make_shared<sofiasip::SuRoot>();
	const auto b2buaServer = std::make_shared<B2buaServer>(b2buaLoop, proxy.getConfigManager());
	b2buaServer->init();
	CoreAssert asserter{proxy, *b2buaLoop};

	const auto& sipProviders =
	    dynamic_cast<const b2bua::bridge::SipBridge&>(b2buaServer->getApplication()).getProviders();
	BC_HARD_ASSERT_CPP_EQUAL(sipProviders.size(), 1);
	const auto& accountPool = sipProviders[0].getAccountSelectionStrategy().getAccountPool();
	// Leave it time to connect to Redis, then load accounts
	asserter
	    .iterateUpTo(
	        10,
	        [&accountPool] {
		        FAIL_IF(accountPool.size() != 3);
		        for (const auto& [_, account] : accountPool) {
			        FAIL_IF(!account->isAvailable());
		        }
		        // b2bua accounts registered
		        return ASSERTION_PASSED();
	        },
	        200ms)
	    .assert_passed();
	BC_HARD_ASSERT_CPP_EQUAL(accountPool.size(), 3);
	{
		const auto& account = accountPool.getAccountByUri("sip:account1@some.provider.example.com");
		BC_HARD_ASSERT(account != nullptr);
		BC_ASSERT_CPP_EQUAL(account->getAlias().str(), "sip:alias@sip.example.org");
	}
	{
		const auto& account = accountPool.getAccountByUri("sip:account2@some.provider.example.com");
		BC_HARD_ASSERT(account != nullptr);
		const auto& authInfo =
		    account->getLinphoneAccount()->getCore()->findAuthInfo("", "account2", "some.provider.example.com");
		BC_HARD_ASSERT(authInfo != nullptr);
		BC_ASSERT_CPP_EQUAL(authInfo->getUserid(), "test-userID");
		BC_ASSERT_CPP_EQUAL(authInfo->getPassword(), "clear text passphrase");
	}
	BC_HARD_ASSERT(accountPool.getAccountByUri("sip:account3@some.provider.example.com") != nullptr);

	// shutdown / cleanup
	std::ignore = b2buaServer->stop();
}

/** Everything is setup correctly except the "From" header template contains a mistake that resolves to an invalid uri.
    Test that the B2BUA does not crash, and simply declines the call.
*/
void invalidUriTriggersDecline() {
	TempFile providersJson{R"json({
		"schemaVersion": 2,
		"providers": [
			{
				"name": "Stub Provider Name",
				"triggerCondition": { "strategy": "Always" },
				"accountToUse": { "strategy": "Random" },
				"onAccountNotFound": "decline",
				"outgoingInvite": {
					"to": "{account.alias}",
					"from": "{account.alias.user};woops=invalid-uri"
				},
				"accountPool": "ExamplePoolName"
			}
		],
		"accountPools": {
			"ExamplePoolName": {
				"outboundProxy": "<sip:stub@example.org>",
				"registrationRequired": false,
				"maxCallsPerLine": 55,
				"loader": [
					{
						"uri": "sip:b2bua-account@example.org",
						"alias": "sip:valid@example.org"
					}
				]
			}
		}
	})json"};
	Server proxy{{
	    // Requesting bind on port 0 to let the kernel find any available port
	    {"global/transports", "sip:127.0.0.1:0;transport=tcp"},
	    {"module::Registrar/enabled", "true"},
	    {"module::Registrar/reg-domains", "example.org"},
	    {"b2bua-server/application", "sip-bridge"},
	    {"b2bua-server/transport", "sip:127.0.0.1:0;transport=tcp"},
	    {"b2bua-server::sip-bridge/providers", providersJson.getFilename()},
	}};
	proxy.start();
	const auto b2buaLoop = std::make_shared<sofiasip::SuRoot>();
	const auto& config = proxy.getConfigManager();
	const auto b2buaServer = std::make_shared<B2buaServer>(b2buaLoop, config);
	b2buaServer->init();
	config->getRoot()
	    ->get<GenericStruct>("module::Router")
	    ->get<ConfigStringList>("static-targets")
	    ->set("sip:127.0.0.1:" + std::to_string(b2buaServer->getTcpPort()) + ";transport=tcp");
	proxy.getAgent()->findModule("Router")->reload();
	const auto caller = ClientBuilder(*proxy.getAgent()).build("caller@example.org");
	CoreAssert asserter{proxy, *b2buaLoop, caller};

	caller.invite("b2bua-account@example.org");
	BC_ASSERT(asserter
	              .iterateUpTo(
	                  2,
	                  [&caller] {
		                  FAIL_IF(caller.getCurrentCall() != std::nullopt);
		                  // invite declined
		                  return ASSERTION_PASSED();
	                  },
	                  400ms)
	              .assert_passed());

	std::ignore = b2buaServer->stop();
}

/** Test (un)registration of accounts against a proxy that requires authentication.
 *
 * A Flexisip proxy will play the role of an external proxy requiring authentication on REGISTERs.
 * The B2BUA is configured with 2 statically defined accounts, one with the full clear-text password, the other with
 * only the HA1.
 * Test that both auth methods are succesful, and that accounts are un-registered properly when the B2BUA server shuts
 * down gracefully.
 *
 * The proxy is configured to challenge every request without exception, meaning the client cannot simply send the
 * unREGISTER and delete everything, but has to respond to the proxy's challenge response.
 */
void authenticatedAccounts() {
	const auto domain = "example.org";
	const auto password = "a-clear-text-password";
	char ha1[33];
	belle_sip_auth_helper_compute_ha1("ha1-md5", "example.org", password, ha1);
	StringFormatter jsonConfig{R"json({
		"schemaVersion": 2,
		"providers": [
			{
				"name": "Authenticate accounts",
				"triggerCondition": { "strategy": "Always" },
				"accountToUse": { "strategy": "Random" },
				"onAccountNotFound": "decline",
				"outgoingInvite": { "to": "{incoming.to}" },
				"accountPool": "RegisteredAccounts"
			}
		],
		"accountPools": {
			"RegisteredAccounts": {
				"outboundProxy": "<sip:127.0.0.1:port;transport=tcp>",
				"registrationRequired": true,
				"maxCallsPerLine": 1,
				"loader": [
					{
						"uri": "sip:cleartext@domain",
						"secretType": "clrtxt",
						"secret": "password"
					},
					{
						"uri": "sip:ha1-md5@domain",
						"secretType": "md5",
						"secret": "md5"
					}
				]
			}
		}
	})json",
	                           '', ''};
	TempFile providersJson{};
	TempFile authDb{R"(version:1

cleartext@example.org clrtxt:a-clear-text-password ;
ha1-md5@example.org clrtxt:a-clear-text-password ;

)"};
	Server proxy{{
	    // Requesting bind on port 0 to let the kernel find any available port
	    {"global/transports", "sip:127.0.0.1:0;transport=tcp"},
	    {"module::Registrar/enabled", "true"},
	    {"module::Registrar/reg-domains", domain},
	    {"b2bua-server/application", "sip-bridge"},
	    {"b2bua-server/transport", "sip:127.0.0.1:0;transport=tcp"},
	    {"b2bua-server::sip-bridge/providers", providersJson.getFilename()},
	    {"module::Authentication/enabled", "true"},
	    {"module::Authentication/auth-domains", domain},
	    {"module::Authentication/db-implementation", "file"},
	    {"module::Authentication/file-path", authDb.getFilename()},
	    // Force all requests to be challenged, even un-REGISTERs
	    {"module::Authentication/nonce-expires", "0"},
	}};
	proxy.start();
	providersJson.writeStream() << jsonConfig.format({
	    {"port", proxy.getFirstPort()},
	    {"domain", domain},
	    {"password", password},
	    {"md5", ha1},
	});
	const auto b2buaLoop = std::make_shared<sofiasip::SuRoot>();
	const auto b2buaServer = std::make_shared<B2buaServer>(b2buaLoop, proxy.getConfigManager());
	b2buaServer->init();

	CoreAssert(proxy, *b2buaLoop)
	    .iterateUpTo(
	        5,
	        [&sipProviders =
	             dynamic_cast<const b2bua::bridge::SipBridge&>(b2buaServer->getApplication()).getProviders()] {
		        for (const auto& provider : sipProviders) {
			        for (const auto& [_, account] : provider.getAccountSelectionStrategy().getAccountPool()) {
				        FAIL_IF(!account->isAvailable());
			        }
		        }
		        // b2bua accounts registered
		        return ASSERTION_PASSED();
	        },
	        70ms)
	    .assert_passed();

	// Graceful async shutdown (unREGISTER accounts)
	const auto& asyncCleanup = b2buaServer->stop();
	const auto& registeredUsers =
	    dynamic_cast<const RegistrarDbInternal&>(proxy.getRegistrarDb()->getRegistrarBackend()).getAllRecords();
	BC_ASSERT_CPP_EQUAL(registeredUsers.size(), 2);
	constexpr static auto timeout = 500ms;
	// As of 2024-03-27 and SDK 5.3.33, the SDK goes on a busy loop to wait for accounts to unregister, instead of
	// waiting for iterate to be called again. That blocks the iteration of the proxy, so we spawn a separate cleanup
	// thread to be able to keep iterating the proxy on the main thread (sofia aborts if we attempt to step the main
	// loop on a non-main thread). See SDK-136.
	const auto& cleanupThread = std::async(std::launch::async, [&asyncCleanup = *asyncCleanup]() {
		BcAssert()
		    .iterateUpTo(
		        1, [&asyncCleanup]() { return LOOP_ASSERTION(asyncCleanup.finished()); }, timeout)
		    .assert_passed();
	});
	CoreAssert(proxy)
	    .iterateUpTo(
	        10, [&registeredUsers] { return LOOP_ASSERTION(registeredUsers.size() == 0); }, timeout)
	    .assert_passed();
	proxy.getRoot()->step(1ms);
	// Join proxy iterate thread. Leave ample time to let the asserter time-out first.
	cleanupThread.wait_for(10s);
	BC_ASSERT_CPP_EQUAL(registeredUsers.size(), 0);
}

TestSuite _{
    "b2bua::bridge",
    {
        CLASSY_TEST(bidirectionalBridging),
        CLASSY_TEST(loadAccountsFromSQL),
        CLASSY_TEST(invalidUriTriggersDecline),
        CLASSY_TEST(authenticatedAccounts),
    },
};
} // namespace
} // namespace flexisip::tester
