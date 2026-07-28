.. _config_yaml:

YAML Configuration File
=======================

The zRPC configuration file is where :ref:`channels <concept_channel>` ---
along with the RPCs that may be sent over them --- are defined. The file is
in YAML format and typically identical across all endpoints participating in
RPC communication.

The configuration file contains a sequence of RPC :ref:`channels <config_channel>`,
each of which contains a sequence of :ref:`RPCs <config_rpc>` sent over it. The latter,
in turn, contain zero or more :ref:`parameters <config_parameter>`.

The following shows a small configuration example defining a single RPC channel
named ``rpc0`` that has identifier ``0``. This channel supports a single RPC named
``set_time`` which originates in the :ref:`host <concept_host>`. This RPC takes
one a single ``uint64_t`` parameter holding the timestamp to be used to update the
RPC :ref:`remote's <concept_remote>` clock.

.. code-block:: yaml
	:caption: Configuration example

	channels:
	  - name: "rpc0"
	    id: 0
	    rpcs:
	      - name: set_time
		parameters:
		  - name: timestamp
		    type: uint64_t
		    description: The correct time.
		brief: Set time of the peer system.
		description: |
		  Update the peer's clock from the provided timestamp.
		origin: [ host ]
	     description: |
	       RPC channel 0.

From the above excerpt, the zRPC core generates a function with the following
signature on the host :ref:`endpoint <concept_endpoint>`.

.. code-block:: c
	:caption: ``set_time`` RPC generated for the ``rpc0`` channel on the host side.

	int zrpc_rpc0_set_time(uint64_t timestamp);

The name of the function is synthezied by combining the ``zprc`` prefix with the name
of the channel and the name of the RPC, each part separated by an underscore.

Invoking the above function causes the host endpoint to emit an RPC encoded as descirbed
:ref:`here <binary_format>` across the backend associated with RPC channel 0. See the
description of the :ref:`devicetree configuration <config_dts>` for more information about
how channels and backends are matched. When the remote endpoint receives the RPC, the latter
is decoded and ``zrpc_rpc0_set_time_serve`` invoked. This function has the following signature
has the signature

.. code-block:: c
	:caption: Function servicing the ``set_time`` RPC

	int zrpc_rpc0_set_time_serve(uint64_t timestamp);

and must be implemented by the user on the receiving endpoint.


.. toctree::
   :maxdepth: 2
   :caption: Contents:

   channel
   rpc
   parameter
   kwalify
