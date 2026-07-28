.. _overview:

Overview
========

zRPC allows for executing RPCs over a generic transport medium. RPCs are ordered in
:ref:`channels <concept_channel>`, each of which is associated with a
:ref:`backend <concept_backend>`.

RPCs are defined under their respective channels in a
:ref:`YAML configuration file <config_yaml>`. While building the module, the zRPC
scripts use the contents of this YAML file to :ref:`generate <generator>` API functions
necessary to invoke the respective RPCs.

Consider the following configuration sample.

.. code-block:: yaml
        :caption: Simple configuration sample

        channels:
          - name: ch0
            id: 0
            rpcs:
              - name: notify_event
                parameters:
                  - name: ev_type
                    type: uint32_t
                    description: |
                      Event to send to the peer.
                brief: Notify host about event.
                description: |
                  Notify the host that an event of type @p ev_type has occured.
                origin: [ remote ]

This declares a single RPC channel named ``ch0`` with identifier ``0``. Said channel
supports a single RPC named ``notify_event`` which is used by the
:ref:`remote <concept_remote>` to notify the :ref:`host <concept_host>` of some event
``ev_type`` occurring. Refer to the :ref:`YAML file reference <config_yaml>` for an
in-depth description of the format.

The above configuration will result in the zRPC build system generating a function named
``zrpc_ch0_notify_event`` in the remote system which, when called, issues the
``notify_event`` RPC. The signature of said function is

.. code-block:: c
        :caption: The generated ``zrpc_ch0_notify_event`` signature.

	int zrpc_ch0_notify_event(uint32_t ev_type);

Naturally, the receiving endpoint --- in this case the host --- must implement a function
used to handle the incomnig RPC. zRPC refers to such handler functions as
:ref:`servicers <concept_servicer>`. A servicer has the exact same signature as its corresponding
RPC [1]_ save for the suffix ``_serve`` being appended to the name. A trivial event handler
servicer might look something akin to

.. code-block:: c
	:caption: ``notify_event`` servicer implemented on the zRPC host.

	int zrpc_ch0_notify_event_serve(uint32_t ev_type)
	{
		LOG_DBG("Peer reported event 0x%" PRIx32, ev_type);

		/* Handle the event */
		return post_event(ev_type);
	}

As is likely immediately apparent, the names of the generated functions are synthesized
from the name of the channel, the name of the RPC along with the ``zrpc`` prefix. Refer to
the :ref:`name synthesis section <synthesis>` for a more details on the naming scheme.

Each channel supports up to 65535 distinct RPCs.

.. [1] Requesting a user pointer as described :ref:`here <rpc_want_user_data>` alters the
   signature by appending a single `void *` pointer to the parameter list.

.. _reporting_errors:

Reporting Errors Back to Peer
-----------------------------

Naturally, the endpoint in which an RPC originates will likely want to know whether or not
an error occurred when its peer processed the RPC. zRPC allows sharing such information
seamlessly --- simply return a negative ``errno`` value from the RPC
:ref:`servicer <concept_servicer>` and this will be returned from the RPC call in the origin
endpoint.

Consider the following RPC servicer

.. code-block:: c
	:caption: RPC servicer queueing up fixed-sized memory blocks in the receiving endpoint.

	K_MSGQ_ALIGN(msgq, sizeof(uint8_t const *), 8, alignof(uint8_t const *));

	int zrpc_ch0_put_data_serve(uint8_t const *data)
	{
		return k_msgq_put(&msgq, data, K_NO_WAIT);
	}

If the :ref:`origin <concept_origin>` invokes the ``zrpc_ch0_put_data`` RPC at a rate higher than
that with which messages are extracted from ``msgq``, the latter will eventually become full at which
point the call to ``k_msgq_put`` fails with ``-ENOMSG``. In such an even, this return value is
embedded in the RPC response and returned from the ``zrpc_ch0_put_data`` function invoked in the
origin.

.. note::

	This behavior causes the origin to block until the receiving endpoint has finished processing
	the RPC and the reply has found its way back to the origin endpoint. For certain applications,
	such delays may be unacceptable and/or unnecessary. For such use cases, the zRPC core can be
	configured to omit replies for specified RPCs using the :ref:`want_reply <rpc_want_reply>` RPC
	property.
