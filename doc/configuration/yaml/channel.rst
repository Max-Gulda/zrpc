.. _config_channel:

Channels
========

The ``channels`` sequence is the topmost entry in the zRPC configuration file. Each
entry therein specifies a unique RPC channel which requires a name, a numeric
identifier and a sequence of :ref:`RPCs <config_rpc>`. Additionally, the channel may
also be associated with a description.

.. _config_channel_name:

Name
----

:key: ``name``
:type: string
:required: yes

The ``name`` entry specifies the name of the channel. Each channel must have a unique name
that itself is a valid `C identifier <https://en.cppreference.com/c/language/identifier>`__.

Identifier
----------

:key: ``id``
:type: int
:required: yes

The channel identifier ``id`` is used to match channels declared in the configuration with
zRPC backends as described :ref:`here <config_dts>`. This identifier must be unique amongst
all channels and should match the value of the ``zrpc,channel-id`` property of exactly one
device node corresponding to a zRPC backend.

RPCs
----

:key: ``rpcs``
:type: Sequence of :ref:`RPCs <config_rpc>`
:required: yes (may be empty)

The ``rpcs`` sequence declares the RPCs defined for the channel. Each entry therein corresponds
to a single RPC that is sent either from the :ref:`host <concept_host>` to the :ref:`remote <concept_remote>`,
vice versa, or in both directions. Refer to the :ref:`RPC <config_rpc>` section for more information.

Description
-----------

:key: ``description``
:type: string
:required: no

Optional description of the channel.
