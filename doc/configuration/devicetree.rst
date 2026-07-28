.. _config_dts:

Devicetree
==========

The devicetree serves two purposes as it pertains to zRPC, namely

#. Selecting the :ref:`backend <concept_backend>` over which RPCs defined
   in the :ref:`configuration file <config_yaml>` are to be sent.
#. Configuring the backend appropriately for the particular system and use
   case.

The device node may, naturally, include whichever properties are required by the
particular backend. Common for all channels is, however, that each node must
set the ``zrpc,channel-id`` integral property. This is used by the zRPC core to
identify the appropriate backend for incoming and outgoing RPCs. Additionally, all
zRPC device nodes **must** include ``"zrpc,channel"`` in their ``compatible`` array
to allow the core to perform this identification.

It should be noted that the ``"zrpc,channel"`` does not single out a backend. As such,
the ``compatible`` array of a backend device node always contains at least two strings ---
one specific to whichever backend is used and the other being the aforementioned
``"zrpc,channel"``.

To spell it out, the core matches the channel named ``primary`` in the following
configuration excerpt

.. code-block:: yaml
	:caption: Configuration excerpt defining an RPC channel named *primary*

	channels:
	  - name: primary
	    id: 0  # Should match zrpc,channel-id
	    rpcs:
	      # ...

against the first device with ``zrpc,channel`` in the ``compatible`` array and
``zrpc,channel-id = <0>;`` set in its corresponding device node.

Matching channels and backends via IDs in this fashion allows the zRPC core to support
multiple combinations of channels and backends simultaneously. This approach even allows
for associating distinct channels with different instances of the same backend.

An RPC device node may be described in  a fashion akin to

.. literalinclude:: /../dts/bindings/rpc/zrpc,channel.yaml
	:caption: zRPC device node sample
	:language: dts
	:start-at: &sram4

This device --- backed by shared memory --- is associated with RPC channel ``0`` and
is configured in :ref:`host <concept_host>` mode.

.. note::

	The devicetree excerpt shown above lacks a number of properties required by the virtio
	backend. Refer to the :ref:`page <backend_virtio>` dedicated to said backend for more
	details on the properties omitted here.

