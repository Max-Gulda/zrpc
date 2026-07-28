.. _concepts:

Concepts
========

The zRPC implementation references a number of concepts that are beneficial to be
familiar with before tackling the rest of this documentation. Some of these will
no doubt be familiar to most users. Yet, understanding how they are used in relation
to zRPC should nevertheless help facilitate understanding.

.. _concept_backend:

Backend
-------

A zRPC backend is a device driver that concretizes the :ref:`channel <concept_channel>`
concept in that it connects the two :ref:`endpoints <concept_endpoint>` to one another via
some sort of communication medium. In practice, the backend implements whatever is required to
allow the two endpoints to send and receive data to and from each other.

.. _concept_channel:

Channel
-------

An RPC channel is an arbitrary association between two :ref:`endpoints <concept_endpoint>`
and a set of RPCs that may be sent between the two. In rough terms, a channel may be thought of as
an abstract tube through which the two endpoints may send objects of a certain shape.
Said objects correspond to the RPCs defined for the channel.

Channels are always running on top of a :ref:`backend <concept_backend>`.

.. _concept_endpoint:

Endpoint
--------

An endpoint is a system --- be it a distinct board, a CPU within a microprocessor or
even a network interface --- in which a :ref:`channel <concept_channel>` terminates.
An endpoint uses said channel to communicate with its :ref:`peer <concept_peer>`.

.. _concept_host:

Host
----

The zRPC host is the :ref:`endpoint <concept_endpoint>` that, in some abstract
fashion, controls the :ref:`channel <concept_channel>` over which it sends and receives
its RPCs.

While the main purpose of distinguishing between a channel host and
:ref:`remote <concept_remote>` is to determine which RPCs are to be sent in which
direction, certain :ref:`backends <concept_backend>` may use the host status for
other purposes as well.

An endpoint is marked as host by the presence of the ``zrpc,host`` ``boolean`` property
in its device node.

.. note::

	The channel host is **not** another term for the RPC server one might be
	familiar with from e.g. `gRPC <https://grpc.io/>`__. zRPC is bidirectional
	in the sense that either both or neither of the endpoints should be thought of
	as a server.

.. _concept_origin:

Origin
------

The origin refers to the :ref:`endpoint <concept_endpoint>` in which an RPC originates.

.. _concept_peer:

Peer
----

Just as in networking, or communication in general for that matter, the term peer
refers to an entity with which a given :ref:`endpoint <concept_endpoint>` communicates.

.. _concept_remote:

Remote
------

The remote is the :ref:`channel <concept_channel>` :ref:`endpoint <concept_endpoint>` that is,
in some sense, subordinate to the channel :ref:`host <concept_host>`. While the primary use
for the designation is to determine which direction over a channel RPCs are to be sent, some
:ref:`backends <concept_backend>` may assign extra responsibilities to the channel remote.

An endpoint is marked as remote by the absence of the ``zrpc,host`` ``boolean`` property
in its device node.

.. note::

	Just as the channel host both is and is not an RPC server, a channel remote both is
	and is not an RPC client. As oxymoronic as that may seem, zRPC does not operate using
	the client-server model employed by most RPC implementations at all. Instead, it
	supports truly bidirectional RPCs.

.. _concept_servicer:

Servicer
--------

An RPC servicer is a function that processes an incoming RPC in the receiving
:ref:`endpoint <concept_endpoint>`. The signature matches that of the RPC save for the
suffix ``_service`` having been appended to the name.

RPC servicers must be implemented by the user.
