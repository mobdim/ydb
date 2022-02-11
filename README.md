<img width="64" src="ydb/docs/_assets/logo.svg"/><br/>

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://github.com/ydb-platform/ydb/blob/main/LICENSE)
[![PyPI version](https://badge.fury.io/py/ydb.svg)](https://badge.fury.io/py/ydb)
[![Telegram](https://img.shields.io/badge/chat-on%20Telegram-2ba2d9.svg)](https://t.me/yandexdatabase_ru)

## YDB Platform

[Website](https://ydb.tech) |
[Documentation](https://ydb.tech/docs) |
[Official Repository](https://github.com/ydb-platform/ydb) |
[YouTube Channel](https://www.youtube.com/channel/UCHrVUvA1cRakxRP3iwA-yyw)

YDB is an open-source Distributed SQL Database that combines high availability and scalability with strict consistency and ACID transactions.

<p align="center">
  <a href=""><img src="ydb/docs/_assets/ydb-promo-video.png" width="70%"/></a>
</p>

## Main YDB Advantages

YDB was designed from scratch as a response to growing demand for scalable interactive web services. Scalability, strict consistency and effective cross-row transactions were a must for such OLTP-like workload. YDB was built by people with strong background in databases and distributed systems, who had an experience of developing No-SQL database and the Map-Reduce system for the one of the largest search engine in the world.
We found that YDB's flexibles design allows us to build more services on top of it including persistent queues and virtual block devices.

Basic YDB features:

  - Fault-tolerant configuration that survive disk, node, rack or even datacenter outage;
  - Horizontal scalability;
  - Automatic disaster recovery with minimum latency disruptions for applications;
  - SQL dialect (YQL) for data manipulation and scheme definition;
  - ACID transactions across multiple nodes and tables with strict consistency.

### Fault-tolerant configurations

YDB could be deployed in three different availability zones. Cluster remains both read and write available during complete outage of a single zone.

Availability zones and regions are covered in more detail [in documentation](docs/en/core/concepts/databases.md#regions-az).

### Horizontal scalability

Unlike traditional RDMBS YDB [scales out](https://en.wikipedia.org/wiki/Scalability#Horizontal_or_scale_out) providing developers with capability to simply extends cluster with computation or storage resources to handle increasing load.

Current production installations have more than 10,000 nodes, store petabytes of data and handle more than 100,000 distributed transactions per second.

### Automatic disaster recovery

YDB Platform has built-in automatic recovery in case of a hardware failure. After unpredictable disk, node, rack or even datacenter failure YDB platform remains fully available for read and write load. No manual intervention required.


## Supported platforms

### Minimal system requirements

YDB runs on x86 64bit platforms with minimum 8 GB of RAM.

### Operating systems

We have major experience running production systems on 64-bit x86 machines working under Ubuntu Linux.

For development purposes we test that YDB could be built and run under latest versions of MacOS and Microsoft Windows on a regular basis.

## Getting started in 5 minutes

1. Install YDB using [pre-built executables](ydb/docs/ru/core/getting_started/ydb_local.md), build it from source or [use Docker container](ydb/docs/en/core/getting_started/ydb_docker.md).
1. Install [command line interace](docs/en/core/reference/ydb-cli/index.m) tool to work with scheme and run queries.
1. Start [local cluster](ydb/docs/ru/core/getting_started/ydb_local.md) or container and run [YQL query](ydb/docs/en/core/yql/reference/index.md) via [YDB CLI](docs/en/core/reference/ydb-cli/index.md).
1. Access [Embedded UI](ydb/docs/en/core/maintenance/embedded_monitoring/ydb_monitoring.md) via browser for schema navigation, query execution and other database development related tasks.
1. Run available [example application](ydb/docs/en/core/reference/ydb-sdk/example/example-go.md).
1. Develop an application using [YDB SDK](ydb/docs/en/core/reference/ydb-sdk)


## How to build

### Prerequisites

In order to build ydbd you should have following tools installed on your system:

1. Git command line tool
1. clang 11 or higher
1. python
1. cmake
1. antlr3
1. libantlr3c
1. libantlr3c-dev

### Build process

1. `git clone https://github.com/ydb-platform/ydb.git`
1. `cd ydb/apps/ydbd`
1. `../../../ya make -r -DOPENSOURCE`

## How to deploy

* Deploy a cluster [using Kubernetes](ydb/docs/en/core/deploy/orchestrated/concepts.md).
* Deploy a cluster using [pre-built executables](ydb/docs/ru/core/getting_started/ydb_local.md).

## How to contribute

We are glad to welcome new contributors to YDB Platform project!

1. Please read [contributor's guide](CONTRIBUTING).
2. We can accept your work to YDB Platform after you have read CLA text.
3. Please don't forget to add a note to your pull request, that you agree to the terms of the CLA. More information can be found in [CONTRIBUTING](CONTRIBUTING) file.

## Success stories

See YDB Platform [official web site](https://ydb.tech/) for the latest success stories and projects using YDB Platform.
