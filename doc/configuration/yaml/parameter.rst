.. _config_parameter:

Parameters
==========

The ``parameters`` sequence is used to specify the parameters to be
included in an RPC. Each entry requires a name and a type and may include
an optional description. In addition to this, non-string pointer parameters
require an associated size parameter and support specifying a parameter direction.

Name
----

:key: ``name``
:type: string
:required: yes

The ``name`` entry specifies the name of the RPC parameter and must be unique amongst the
parameters. The string must be a valid
`C identifier <https://en.cppreference.com/c/language/identifier>`__.

Type
----

:key: ``type``
:type: string
:required: yes

Unsurprisingly, the ``type`` entry specifies the type of the RPC parameter. Its value
should be a valid C type declaration, the fundamental type of which is one of the
fixed-width unsigned integers ``uint8_t``, ``uint16_t``, ``uint32_t`` and ``uint64_t`` or
``char``. Signed integers apart from the potentially signed ``char`` may be passed by simply
casting them to an appropriate unsigned version.

.. warning::

	Be aware that signed integer representation was not standardized until C23. While it is highly
	unlikely to encounter a system that uses anything but two's complement, they do nevertheless
	exist.

Apart from simple integral parameters, the zRPC core supports null-terminated strings, pointers to
non-terminated data as well as output parameters.

Integral Parameters
*******************

Declaring an integral parameters is trivial. Simply set the ``type`` property to one of the supported
fundamental types. The parameters may be both ``volatile`` and --- as useless as it is --- ``const``
qualified.

Integral parameters are used solely as input.

Null-Terminated String
**********************

The zRPC core treats any parameter with the type ``char *`` or a cv-qualified version thereof as a
null-terminated string. As one might expect, the default means of the determining the size of a string
by the invoking the equivalent of ``strlen``. Alternatively, one may associate the string with a
:ref:`size <param_size>` in which case the zRPC core ignores potential null-terminators and uses the
configured size instead. This is especially useful when using strings as output parameters.

.. warning::

	Using fixed-size strings as input parameters causes the zRPC core to copy however many bytes
	has been configured from the address passed to the RPC. Make sure that this string is large enough
	lest you will run the risk of everything from page faults to disclosing sensitive information.

Null-temrminated string parameters may be used as both input and output parameters. The latter does,
for obvious reasons, require that the parameter is not ``const`` qualified. For output parameters, the
:ref:`servicer <concept_servicer>` should abide by the following constraints

#. It **must not** write beyond the end of the string buffer. If the input is null-terminated, the servicer
   should use ``strlen`` or equivalent to determine how many bytes it may write to the buffer. If the string
   is fixed-size, it the servicer should use the designated size parameter instead of ``strlen``.
#. It **must** ensure that the reply to a NULL-terminated input --- i.e. an input string that is not fixed-size ---
   is itself NULL-terminated.

.. warning::

	Terminating an output string is **truly** required if the input buffer was itself null-terminated as the
	zRPC core leverages ``strlen`` to determine the size of all non-fixed-size output strings.

Non-Terminated Pointer
**********************

Non-terminated pointers are slightly more cumbersome to use in that they require a :ref:`size <param_size>`
parameter specifying how many elements are available at the address. This is true also for pointers to single
elements used as output parameters.

The zRPC core supports both pointer to ``const``, pointer to ``volatile`` and pointer to ``const volatile``.
Pointer parameters may be used for both input and output.

.. _param_size:

Size
----

:key: ``size``
:type: string
:required: For non-terminated pointers parameters, optional for strings.

The ``size`` field holds the name of an integral parameter passed to the same RPC. This integral parameter
should contain the number of elements that are available at the parameter for which the ``size`` field is
set. For example, if one were to do the admittedly nonsensical thing of implementing a remote version of
``memset``, the configuration entry would look something like

.. code-block:: yaml
	:caption: Size parameter illustration, a remote ``memset``.
	:emphasize-lines: 11,20

	channels:
	  - name: ch0
	    id: 0
	    rpcs:
	      - name: very_slow_memset
		parameters:
		  - name: s
		    # The core doesn't support void *, use uint8_t * instead
		    type: uint8_t *
		    # The parameter n holds the size of this array
		    size: n
		    description: |
		      Address to write to.
		  - name c
		    # Second parameter to memset is an int, core supports only
		    # fixed-width unsigned types
		    type: uint32_t
		    description: |
		      Value to write to each byte at @p s.
		  - name: n
		    # size_t is not supported, use uint32_t instead.
		    type: uint32_t
		    description: |
		      Number of elements at @p s.

The above configuration instructs the zRPC generation scripts that there are ``n``
elements at the address held in ``s``.

The size parameter should hold the number of elements in the array referred to by the
pointer for which the size parameter is set. This means that, assuming the size parameter
is named ``n`` and the fundmental type of the associated pointer is ``uint8_t``, the zRPC
core will process exactly ``n`` bytes. If, instead, the fundamental type were ``uint32_t``,
the core would process ``n * sizeof(uint32_t)`` bytes.

In an attempt to dispel any doubts about the above, consider the following specification

.. code-block:: yaml
	:caption: Parameter size vis-à-vis fundamental type

	channels:
	  - name: ch0
	    id: 0
	    rpcs:
	      - name: send_a_bunch_of_data
		parameters:
		  - name: u8s
		    type: uint8_t const *
		    size: u8s_size
		  - name: u16s
		    type: uint16_t const *
		    size: u16s_size
		  - name: u32s
		    type: uint32_t const *
		    size: u32s_size
		  - name: u8s_size
		    type: uint32_t
		  - name: u16s_size
		    type: uint32_t
		  - name: u32s_size
		    type: uint32_t

Assuming this RPC is invoked with the same value ``n`` passed to each size parameter
``u8s_size``, ``u16s_size`` and ``u32s_size``, the RPC will include ``n`` bytes from
the address in ``u8s``, ``2 * n`` bytes read from the address in ``u16s`` and ``4 * n``
bytes from the address in ``u32s``.

Reducing Duplication Using YAML Anchors
***************************************

Having to specify the name of each size parameter in two or more places comes with the risk of the
names diverging as a result of future changes. While this should be caught by the generation scripts,
it remains an annoying eventuality, one that can be circumvented entirely by using YAML anchors. The
following declares the same overcomplicated ``memset`` variant as above but uses a YAML anchor to
declare the name of the size parameter in only one place. Instead of naming the parameter again,
the second occurrence simply dereferences the anchor.

.. code-block:: yaml
	:caption: The inefficient ``memset`` again, this time with anchors and references.
	:emphasize-lines: 9, 16

	channels:
	  - name: ch0
	    id: 0
	    rpcs:
	      - name: very_slow_memset
		parameters:
		  - name: s
		    type: uint8_t *
		    size: &s-size n
		    description: |
		      Address to write to.
		  - name c
		    type: uint32_t
		    description: |
		      Value to write to each byte at @p s.
		  - name: *s-size
		    type: uint32_t
		    description: |
		      Number of elements at @p s.

Description
-----------

:key: ``description``
:type: string
:required: no

Optional description passed to the ``@param`` Doxygen function.

Direction
---------

:key: ``direction``
:type: string
:required: no

The ``direction`` property instructs the zRPC generation scripts on how the parameter is
to be used. This may be either as input, as output or as both. Only non-``const`` pointer
parameters may be used for output.

Valid choices are ``in``, ``out`` and ``inout``. Integral types and ``const`` qualified pointers
default to ``in``, remaining parameters to ``inout``.
