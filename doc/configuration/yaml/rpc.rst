.. _config_rpc:

RPCs
====

The ``rpcs`` sequence present in all :ref:`channel <config_channel>` entries
in in the zRPC configuration file lists the RPCs that may be sent over said
channel. Each RPC must be given a name. Additionally, it may support parameters
and be associated with both a brief and a more extensive description. Furthermore,
it requires a list of :ref:`endpoints <concept_endpoint>` from which the RPC may
be emitted. Lastly, it supports a number of flags that alter the zRPC core
behavior on a per-RPC level.

Name
----

:key: ``name``
:type: string
:required: yes

The ``name`` entry specifies the name of the RPC and must be unique within the channel. Much
like the channel name, the RPC name must itself be a valid
`C identifier <https://en.cppreference.com/c/language/identifier>`__.

Parameters
----------

:key: ``parameters``
:type: Sequence of :ref:`parameters <config_parameter>`.
:required: no

The ``parameters`` sequence specifies the parameters required by the RPC. Omitting this
sequence or, alternatively, leaving it empty, yields a nullary function. Refer to the
:ref:`parameter <config_parameter>` section for more information on each entry.

Brief
-----

:key: ``brief``
:type: string
:required: no

The string associated with the ``brief`` key is included in the documentation generated
by the zRPC scripts. More specifically, it is passed to the ``@brief`` Doxygen function.

Description
-----------

:key: ``description``
:type: string
:required: no

The ``description`` string is, much like the ``brief`` counterpart, used in the generated
Doxygen documentation. More specifically, it is used with the equivalent of Doxygen's
``@details``.

Origin
------

:key: ``origin``
:type: Sequence of strings
:required: yes

The ``origin`` sequence is used to determine from which endpoint the RPC may originate. The
sequence may contain any combination of the strings ``host`` and ``remote``. When the former is
set, the RPC may be sent from the :ref:`host <concept_host>` whereas when the latter is included,
the RPC may be sent from the :ref:`remote <concept_remote>`.


.. _rpc_want_reply:

Want Reply
----------

:key: ``want_reply``
:type: bool
:required: no

As noted under :ref:`Reporting Errors <reporting_errors>`, invoking an RPC will normally cause
the calling endpoint to block until either a response has been received --- indicating that the
RPC has completed --- or said endpoint times out. This behavior is ill-suited to applications in
which RPCs must be invoked with high frequency, especially if there is no way of recovering from
an error anyway. This is where the ``want_reply`` property comes in handy.

Including ``want_reply: False`` in an RPC mapping alters the behavior of the RPC core in a fashion
such that it omits the call to ``zrpc_recv`` altogether. Instead, the RPC is sent in a fire-and-forget
fashion. Naturally, the value returned from an RPC with ``want_reply`` set to ``False`` no longer
reflects the value returned by the peer servicer. Instead, it simply indicates whether or not the RPC
could be sent.

The option defaults to ``True``.

.. _rpc_want_user_data:

Want User Data
--------------

:key: ``want_user_data``
:type: bool
:required: no

The ``want_user_data`` allows the user to associate an arbitrary pointer with the RPC servicer.
Including ``want_user_data`` as done in the following configuration

.. code-block:: yaml
	:caption: An RPC requesting user data association.
	:emphasize-lines: 21,22

	channels:
	  - name: "ch0"
	    id: 0
	    rpcs:
	      - name: udp_forward
		parameters:
		  - name: data
		    type: uint8_t const *
		    description: Data to be forwarded
		    size: &data-size size
		  - name: *data-size
		    type: uint32_t
		    description: |
		      Number of bytes at @p data to be sent.
		brief: Forward arbitrary data over the network
		description: |
		  Forward whatever data is received on the RPC channel over
		  the network
		origin: [ host ]

		# Request user pointer in handler
		want_user_data: True

	     description: |
	       RPC channel 0.

alters the signature of the servicer from

.. code-block:: c
	:caption: Signature without user data

	int zrpc_ch0_udp_forward_serve(uint8_t const *data, uint32_t size)

to

.. code-block:: c
	:caption: Signature with user data requested

	int zrpc_ch0_udp_forward_serve(uint8_t const *data, uint32_t size, void *user_data);

The ``user_data`` pointer --- initialized to ``NULL`` --- is a global part of the generated section of
the zRPC core and may be assigned via the ``zrpc_ch0_udp_forward_set_user_data`` function,
the signature of which is

.. code-block:: c
	:caption: Signature of the function used to set user data for the ``udp_forward`` RPC.

	int zrpc_ch0_udp_forward_set_user_data(void *user_data);

Accesses to the RPC-specific user data pointer --- both assignments through the generated
``rpc_{{ channel.name }}_{{ rpc.name }}_set_user_data`` function and potential accesses via in the RPC
servicer --- are protected by one of ``1 << ZRPC_USER_DATA_BUCKET_SHIFT`` mutexes.

.. note::

	Protecting accesses to the ``user_data`` parameter while executing the RPC servicer requires that
	the zRPC core invokes it with the parameter mutex locked. As such, two incoming RPCs of the same
	type for which ``ẁant_user_data`` is set to ``True`` cannot be serviced concurrently.

The user data feature may be used to share data between the handler and an arbitrary site in a fashion
similar to the following.

.. code-block:: c
	:caption: Somewhat contrived example of how the user data feature may be used

	/* Data to be shared with the handler */
	struct udp_user_data {
		struct net_context *net_ctx;
		struct net_sockaddr dst;
	};

	/* RPC servicer */
	int zrpc_ch0_udp_forward_serve(uint8_t const *data, uint32_t size, void *user_data)
	{
		struct udp_user_data *ud = user_data;
		/* Handler invoked with user data mutex locked, accesses are safe */
		if (!user_data) {
			LOG_WRN("RPC invoked too early");
			return -EAGAIN;
		}

		return net_context_sendto(ud->next_ctx, data, size, &ud->dst
			sizeof(ud.dst), NULL, K_MSEC(1000), NULL);
	}

	int main(void)
	{
		int ret;
		static struct udp_user_data ud;

		/* Error handling omitted */
		net_addr_pton(AF_INET6, "FF02::1", &net_sin6(&ud.dst)->sin6_addr);
		ud.net_ctx = bind_udp_context();

		/* Assignment locks the same mutex that is held when executing
		 * zrpc_ch0_udp_forward_service */
		ret = zrpc_ch0_udp_forward_set_user_data(&ud);
		if (ret)
			LOG_ERR("Could not set user data: %d", -ret);

		/* ... */
	}


The option defaults to ``False``.
