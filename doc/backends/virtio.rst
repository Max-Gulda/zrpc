.. _backend_virtio:

VirtIO
======

The virtio backend allows RPC execution over shared memory. This is intended for
facilitating communication between CPUs on heterogeneous processors such as those
in the STM32H74X and STM32H75X families.

THe backend is enabled by setting the ``CONFIG_ZRPC_BACKEND_VIRTIO`` option.

.. list-table:: Required devicetree properties
	:header-rows: 1

	* - **Property**
	  - **Type**
	  - **Value(s)**
	* - ``compatible``
	  - ``string-array``
	  - ``"zrpc,virtio-channel", "zrpc,channel"``
	* - ``zrpc,channel-id``
	  - ``int``
	  - Identifier used to match the backend with a channel declared
	    in the configuration file.
	* - ``zrpc,virtio-ipm-handle``
	  - ``phandle``
	  - A device implementing the ``ipm_driver_api``.
	* - ``zrpc,host``
	  - ``boolean``
	  - (set for exactly one endpoint per channel)

The VirtIO is built on top of `OpenAMP <https://www.openampproject.org/>`__. More
specifically, it leverages the ring buffers uses the RPMsg message bus which
fundamentally relies on a pair of single-producer, single-consumer ring buffers.

The backend is enabled by declaring a devicetree node with ``"zrpc,virtio-channel"``
in its ``compatible`` array. Additionally, the ``reg``, ``zrpc,channel-id``,
``zrpc,virtio-ipm-handle`` properties are all mandatory. Unsurprisingly, the
former is a two-tuple specifying the base address and the size of the shared memory region
to use. The ``zrpc,channel-id`` holds the numeric channel identifier used to match the
backend against a channel defined in the :ref:`configuration file <configuration>`.

While virtio-backed RPCs are sent --- that is, written to and read from --- shared memory,
the respective endpoints need a way to signal its peer that an RPC is available in the
RX ring of the latter. This is where the ``zrpc,virtio-ipm-handle`` comes in. This phandle
should refer to a device implementing at least the ``ipm_send`` and
``ipm_register_callback`` parts of the ``ipm_driver_api``.

.. note::

	Apart from the ``"zrpc,virtio-channel"``, the ``compatible`` array must
	always include ``"zrpc,channel"`` as well

	.. code-block:: dts
		:caption: Proper ``compatible`` array for a virtio channel

		memory@deadbeef {
			compatible = "zrpc,virtio-channel",
				     "zrpc,channel";

			/* ... */
		};

In addition to the above, exactly one of the virtio node in the node pair of an RPC
channel must set the ``zrpc,host`` property.


.. warning::

	While there is nothing in the virtio backend itself that prevents using multiple
	virtio instances in parallel across distinct memory regions, each such channel
	would require its own IPM handle. Assigning the same handle to two virtio backend
	instances will trigger reads on both instances whenever an RPC arrives on one of
	them.

.. note::

	If you CPU has dcache, you will want to set the ``zrpc,cpu-has-dcache`` property in the
	devicetree node. Doing so automatically enables ``CONFIG_OPENAMP_WITH_DCACHE``.

.. _virtio_host_remote:

Host Selection
--------------

At startup, the virtio backend performs a handshake between the two channel endpoints. This
handshake is initiated by the channel host. Additionally, the host is responsible for
initializing a shared status block early during startup. As such, the virtio backend uses
the ``zrpc,host`` property for more than simply determining RPC direction.

While the backend itself cares little for which endpoint is configured as host, processor
characteristics may make one endpoint more suitable than others. The
:ref:`STM32H755 example <virtio_stm32h755>` below is a good example of where the selection does
matter. On this microprocessor, the Cortex-M7 starts before the Cortex-M4. The latter is allowed
to boot only once the former has completed a not-so-insignificant amount of system initialization.
In a system such as this, the CPU that starts first is typically better suited as the host.

.. _virtio_stm32h755:

Example: STM32H755
------------------

The `STM32H755 <https://www.st.com/en/microcontrollers-microprocessors/stm32h745-755.html>`__
heterogeneous dual-core microprocessor features a Cortex-M4 and a Cortex-M7 processor with
four memory regions labelled ``SRAM1`` through ``SRAM4``. Direct communication between the
two processors is possible only using a pair of hardware semaphores (HSEM) which, when downed
by one CPU triggers an interrupt on the other.

Zephyr systems would typically run distinct images on the two CPUs, each of the former based on
a distinct devicetree. With such a setup, leveraging the virtio backend for communication between
the two CPUs requires that the driver be enabled in both devicetrees. The node in the Cortex-M7
tree might look something akin to

.. literalinclude:: /../dts/bindings/rpc/zrpc,virtio-channel.yaml
	:caption: VirtIO node in the STM32H755 Cortex-M7 devicetree
	:language: dts
	:start-at: /* STM32 HSEM IPM mailbox */
	:end-before: # VirtIO configuration for the STM32H755 Cortex-M4

As can be seen, the configuration uses the entire 64K SRAM4 region for RPCs passed between the
CPUs [1]_ to send RPCs declared for the channel with identifier 0 in the configuration file. The
Cortex-M7 runs in :ref:`host <virtio_host_remote>` mode, uses the ``mailbox`` node ---
corresponding to the ``ipm_stm32_hsem`` driver --- for asynchronous inter-processor communication
and allows RPC blocks of at most 1024 bytes in size [2]_.

Naturally, having configured the Cortex-M7 endpoint, the same must be done for the Cortex-M4
counterpart. The node on this CPU will typically match that of the Cortex-M7 apart from only one
of them being allowed to include the ``zrpc,host`` property. Since the Cortex-M7 did, the property
us omitted on the Cortex-M4.

.. literalinclude:: /../dts/bindings/rpc/zrpc,virtio-channel.yaml
	:caption: VirtIO node in the STM32H755 Cortex-M4 devicetree
	:language: dts
	:start-at: /* Use the same IPM implementation as on the Cortex-M7 */

While it is generally recommended to keep all properties apart from ``zrpc,host`` identical for the
two endpoints, this is strictly necessary for only part of the properties. Refer to the descriptions
in ``dts/bindings/rpc/zrpc,channel.yaml`` and ``dts/bindings/rpc/zrpc,virtio-channel.yaml`` in the
source code for not only a complete list of available properties but also information about which
are allowed to differ between the endpoints.

.. [1] This is quite voracious, suitable for transferring relatively large amounts of data at
   a moderate-to-high rate as one would when e.g. streaming audio.  Most use cases would likely
   require far less.

.. [2] These blocks include an RPMsg header which, at least at the of writing, is 16 bytes. As such,
   a value of 1024 caps the actual RPCs at 1008 bytes. Attempting to send anything larger than this
   results in truncation.
