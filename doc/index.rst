zRPC
====

zRPC is a Zephyr module implementing generic, serverless
`remote procedure calls <https://en.wikipedia.org/wiki/Remote_procedure_call>`__. In stark
contrast to more well-known RPC implementations such as `pw_rpc <https://pigweed.dev/pw_rpc/>`__
and `Apache Thrift <https://thrift.apache.org/>`__, zRPC is designed specifically for Zephyr
and is as such capable of running with a smaller memory footprint.

The module effectively operates on two levels --- the first being the :ref:`RPC generator <generator>`,
the second the :ref:`RPC transport medium <backends>`.

Features
--------

zRPC boasts the following features

- Designed specifically with Zephyr in mind.
- Implemented in native C.
- C++ support.
- Bidirectional RPC execution.
- Transport medium agonostic --- simply select one of the available :ref:`backends <backends>` or
  implement one of your own.
- Suited to high-throughput workloads such as audio streaming [1]_.
- Generic, easily extensible binary format.
- Built-in :ref:`endpoint <concept_endpoint>` compatibility verification.

.. [1] Subject to limitations imposed by the underlying transport medium. A serial port, for example,
   would limit throughput significantly.

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   overview
   concepts
   configuration/index
   generator/index
   backends/index
   binary/index
   building
