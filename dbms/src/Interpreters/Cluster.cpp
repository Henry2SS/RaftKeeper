#include <DB/Interpreters/Cluster.h>
#include <DB/Common/escapeForFileName.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/Application.h>
#include <Poco/Net/NetworkInterface.h>
#include <boost/bind.hpp>

namespace DB
{


Cluster::Address::Address(const String & config_prefix)
{
	auto & config = Poco::Util::Application::instance().config();

	host_port = Poco::Net::SocketAddress(
		config.getString(config_prefix + ".host"),
		config.getInt(config_prefix + ".port")
	);

	user = config.getString(config_prefix + ".user", "default");
	password = config.getString(config_prefix + ".password", "");
}

Cluster::Address::Address(const String & host_port_, const String & user_, const String & password_)
	: user(user_), password(password_)
{
	UInt16 default_port = Poco::Util::Application::instance().config().getInt("tcp_port", 0);

	/// Похоже на то, что строка host_port_ содержит порт. Если условие срабатывает - не обязательно значит, что порт есть (пример: [::]).
	if (nullptr != strchr(host_port_.c_str(), ':') || !default_port)
		host_port = Poco::Net::SocketAddress(host_port_);
	else
		host_port = Poco::Net::SocketAddress(host_port_, default_port);
}

namespace
{
	inline std::string addressToDirName(const Cluster::Address & address)
	{
		return
			escapeForFileName(address.user) +
			(address.password.empty() ? "" : (':' + escapeForFileName(address.password))) + '@' +
			escapeForFileName(address.host_port.host().toString()) + ':' +
			std::to_string(address.host_port.port());
	}
}


Clusters::Clusters(const Settings & settings, const DataTypeFactory & data_type_factory, const String & config_name)
{
	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();
	Poco::Util::AbstractConfiguration::Keys config_keys;
	config.keys(config_name, config_keys);

	for (Poco::Util::AbstractConfiguration::Keys::const_iterator it = config_keys.begin(); it != config_keys.end(); ++it)
		impl.emplace(std::piecewise_construct,
			std::forward_as_tuple(*it),
			std::forward_as_tuple(settings, data_type_factory, config_name + "." + *it));
}


Cluster::Cluster(const Settings & settings, const DataTypeFactory & data_type_factory, const String & cluster_name):
	local_nodes_num(0)
{
	Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();
	Poco::Util::AbstractConfiguration::Keys config_keys;
	config.keys(cluster_name, config_keys);

	const auto & config_prefix = cluster_name + ".";

	for (auto it = config_keys.begin(); it != config_keys.end(); ++it)
	{
		if (0 == strncmp(it->c_str(), "node", strlen("node")))
		{
			const auto & prefix = config_prefix + *it;
			const auto weight = config.getInt(prefix + ".weight", 1);
			if (weight == 0)
				continue;

			addresses.emplace_back(prefix);

			slot_to_shard.insert(std::end(slot_to_shard), weight, shard_info_vec.size());
			if (const auto is_local = isLocal(addresses.back()))
				shard_info_vec.push_back({{}, weight, is_local });
			else
				shard_info_vec.push_back({{addressToDirName(addresses.back())}, weight, is_local});
		}
		else if (0 == strncmp(it->c_str(), "shard", strlen("shard")))
		{
			Poco::Util::AbstractConfiguration::Keys replica_keys;
			config.keys(config_prefix + *it, replica_keys);

			addresses_with_failover.emplace_back();
			Addresses & replica_addresses = addresses_with_failover.back();

			const auto & partial_prefix = config_prefix + *it + ".";
			const auto weight = config.getInt(partial_prefix + ".weight", 1);
			if (weight == 0)
				continue;

			const auto internal_replication = config.getBool(partial_prefix + ".internal_replication", false);

			/** in case of internal_replication we will be appending names to
			 *  the first element of vector; otherwise we will just .emplace_back
			 */
			std::vector<std::string> dir_names{};
			auto has_local_node = false;

			auto first = true;
			for (auto jt = replica_keys.begin(); jt != replica_keys.end(); ++jt)
			{
				if (0 == strncmp(jt->data(), "weight", strlen("weight")) ||
					0 == strncmp(jt->data(), "internal_replication", strlen("internal_replication")))
					continue;

				if (0 == strncmp(jt->c_str(), "replica", strlen("replica")))
				{
					replica_addresses.emplace_back(partial_prefix + *jt);

					if (isLocal(replica_addresses.back()))
					{
						has_local_node = true;
					}
					else
					{
						if (internal_replication)
						{
							auto dir_name = addressToDirName(replica_addresses.back());
							if (first)
								dir_names.emplace_back(std::move(dir_name));
							else
								dir_names.front() += "," + dir_name;
						}
						else
							dir_names.emplace_back(addressToDirName(replica_addresses.back()));

						if (first) first = false;
					}
				}
				else
					throw Exception("Unknown element in config: " + *jt, ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
			}

			slot_to_shard.insert(std::end(slot_to_shard), weight, shard_info_vec.size());
			shard_info_vec.push_back({std::move(dir_names), weight, has_local_node});
		}
		else
			throw Exception("Unknown element in config: " + *it, ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
	}

	if (!addresses_with_failover.empty() && !addresses.empty())
			throw Exception("There must be either 'node' or 'shard' elements in config", ErrorCodes::EXCESSIVE_ELEMENT_IN_CONFIG);

		if (addresses_with_failover.size())
		{
			for (AddressesWithFailover::const_iterator it = addresses_with_failover.begin(); it != addresses_with_failover.end(); ++it)
			{
				ConnectionPools replicas;
				replicas.reserve(it->size());

				bool has_local_replics = false;
				for (Addresses::const_iterator jt = it->begin(); jt != it->end(); ++jt)
				{
					if (isLocal(*jt))
					{
						has_local_replics = true;
						break;
					}
					else
					{
						replicas.emplace_back(new ConnectionPool(
							settings.distributed_connections_pool_size,
							jt->host_port.host().toString(), jt->host_port.port(), "", jt->user, jt->password, data_type_factory, "server", Protocol::Compression::Enable,
							saturate(settings.connect_timeout_with_failover_ms, settings.limits.max_execution_time),
							saturate(settings.receive_timeout, settings.limits.max_execution_time),
							saturate(settings.send_timeout, settings.limits.max_execution_time)));
					}
				}

				if (has_local_replics)
					++local_nodes_num;
				else
					pools.emplace_back(new ConnectionPoolWithFailover(replicas, settings.load_balancing, settings.connections_with_failover_max_tries));
			}
		}
		else if (addresses.size())
		{
			for (Addresses::const_iterator it = addresses.begin(); it != addresses.end(); ++it)
			{
				if (isLocal(*it))
				{
					++local_nodes_num;
				}
				else
				{
					pools.emplace_back(new ConnectionPool(
						settings.distributed_connections_pool_size,
						it->host_port.host().toString(), it->host_port.port(), "", it->user, it->password, data_type_factory, "server", Protocol::Compression::Enable,
						saturate(settings.connect_timeout, settings.limits.max_execution_time),
						saturate(settings.receive_timeout, settings.limits.max_execution_time),
						saturate(settings.send_timeout, settings.limits.max_execution_time)));
				}
			}
		}
		else
			throw Exception("No addresses listed in config", ErrorCodes::NO_ELEMENTS_IN_CONFIG);
}


Cluster::Cluster(const Settings & settings, const DataTypeFactory & data_type_factory, std::vector< std::vector<String> > names,
				 const String & username, const String & password): local_nodes_num(0)
{
	for (size_t i = 0; i < names.size(); ++i)
	{
		Addresses current;
		for (size_t j = 0; j < names[i].size(); ++j)
			current.emplace_back(names[i][j], username, password);
		addresses_with_failover.emplace_back(current);
	}

	for (AddressesWithFailover::const_iterator it = addresses_with_failover.begin(); it != addresses_with_failover.end(); ++it)
	{
		ConnectionPools replicas;
		replicas.reserve(it->size());

		for (Addresses::const_iterator jt = it->begin(); jt != it->end(); ++jt)
		{
			replicas.emplace_back(new ConnectionPool(
				settings.distributed_connections_pool_size,
				jt->host_port.host().toString(), jt->host_port.port(), "", jt->user, jt->password, data_type_factory, "server", Protocol::Compression::Enable,
				saturate(settings.connect_timeout_with_failover_ms, settings.limits.max_execution_time),
				saturate(settings.receive_timeout, settings.limits.max_execution_time),
				saturate(settings.send_timeout, settings.limits.max_execution_time)));
		}
		pools.emplace_back(new ConnectionPoolWithFailover(replicas, settings.load_balancing, settings.connections_with_failover_max_tries));
	}
}


Poco::Timespan Cluster::saturate(const Poco::Timespan & v, const Poco::Timespan & limit)
{
	if (limit.totalMicroseconds() == 0)
		return v;
	else
		return v > limit ? limit : v;
}


bool Cluster::isLocal(const Address & address)
{
	///	Если среди реплик существует такая, что:
	/// - её порт совпадает с портом, который слушает сервер;
	/// - её хост резолвится в набор адресов, один из которых совпадает с одним из адресов сетевых интерфейсов сервера
	/// то нужно всегда ходить на этот шард без межпроцессного взаимодействия
	const UInt16 clickhouse_port = Poco::Util::Application::instance().config().getInt("tcp_port", 0);
	static auto interfaces = Poco::Net::NetworkInterface::list();

	if (clickhouse_port == address.host_port.port() &&
		interfaces.end() != std::find_if(interfaces.begin(), interfaces.end(),
			[&](const Poco::Net::NetworkInterface & interface) { return interface.address() == address.host_port.host(); }))
	{
		LOG_INFO(&Poco::Util::Application::instance().logger(), "Replica with address " << address.host_port.toString() << " will be processed as local.");
		return true;
	}
	return false;
}

}
