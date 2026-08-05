# Copyright (c) 2026 Vilhelm Engström
#
# SPDX-License-Identifier: Apache-2.0

"""zRPC RPC abstractions"""

from __future__ import annotations

import collections
import contextlib
import dataclasses
import enum
import inspect
import json
import os
import pathlib
import re
import tempfile
import warnings

from typing import Any, Dict, List, Tuple, Union

import crc8
import kconfiglib
import pykwalify
import pykwalify.core
import yaml

from pycparser import c_ast, c_parser

from zrpc.error import (
    ChannelIdReuseError,
    DuplicationError,
    InvalidDirectionError,
    InvalidHexDigest,
    InvalidSizeFieldError,
    MissingOriginError,
    MissingSizeFieldError,
    SelfRefError,
    UnsizedFieldError,
)


class classproperty(property):
    def __get__(self, cls, owner) -> Any:  # type: ignore[override]
        return classmethod(self.fget).__get__(None, owner)()  # type: ignore[arg-type]


class IdentType(enum.IntEnum):
    """RPC fundamental parameter type"""

    PTR_BIT = 0x10

    U8 = 0
    U16 = 1
    U32 = 2
    U64 = 3
    CHAR = 4
    VOID = 5
    BINARY8 = PTR_BIT | U8
    BINARY16 = PTR_BIT | U16
    BINARY32 = PTR_BIT | U32
    BINARY64 = PTR_BIT | U64
    NUL_STRING = PTR_BIT | CHAR

    @property
    def fundamental_type(self) -> str:
        """Get fundamental C type"""
        return {
            **{getattr(self, f"U{n}"): f"uint{n}_t" for n in (8, 16, 32, 64)},
            self.CHAR: "char",
        }[IdentType(self.value & ~self.PTR_BIT)]

    @property
    def fundamental_type_size(self) -> str:
        """Get size of fundamental type, in bytes"""
        return {
            **{getattr(self, f"U{n}"): f"{n // 8}" for n in (8, 16, 32, 64)},
            self.CHAR: "1",
        }[IdentType(self.value & ~self.PTR_BIT)]

    @classmethod
    def from_fundamental_type(cls, fundamental: str) -> IdentType:
        """Convert fundamental C type to enumerator.

        :param fundamental: The fundamental C type.
        :type: fundamental: str

        :return: The converted type.
        :rtype: :class:`zrpc.rpc.IdentType`
        """

        return getattr(
            cls,
            {**{f"uint{n}_t": f"U{n}" for n in (8, 16, 32, 64)}, "char": "CHAR"}[
                fundamental
            ],
        )


class Qualifier(enum.IntEnum):
    """Parameter qualifiers"""

    CONST = 1
    VOLATILE = 2


@dataclasses.dataclass(slots=True)
class TypeDecl:
    ident: IdentType
    qualifiers: int

    @property
    def is_ptr(self) -> bool:
        """Determine whether the decl is a pointer"""
        return not not (self.ident & IdentType.PTR_BIT)

    @property
    def is_str(self) -> bool:
        """Determine whether the decl is a string"""
        return self.ident == IdentType.NUL_STRING

    @property
    def is_const_qualified(self) -> bool:
        """Determine whether or not the decl is const-qualified"""
        return not not (self.qualifiers & Qualifier.CONST.value)

    @property
    def is_volatile_qualified(self) -> bool:
        """Determine whether or not the decl is const-qualified"""
        return not not (self.qualifiers & Qualifier.VOLATILE.value)

    @property
    def c_decl(self) -> str:
        """Get snippet declaring the type"""
        qualifiers = [q.name.lower() for q in Qualifier if q.value & self.qualifiers]
        return "".join(
            filter(
                lambda s: s,
                [
                    self.ident.fundamental_type,
                    (" " + " ".join(qualifiers) if qualifiers else ""),
                    " *" if self.is_ptr else " ",
                ],
            )
        )

    @property
    def c_decl_unqual(self) -> str:
        """Get snippet declaring the type, qualifiers exlucded"""
        return self.ident.fundamental_type + (" *" if self.is_ptr else " ")

    @classmethod
    def from_yaml(cls, typedecl: str) -> TypeDecl:
        """Parse parameter type from expression

        :param typedecl: C-style type declaration for the parameter.
        :type typedecl: str

        :return: The type of the parameter.
        :rtype: :class:`zrpc.rpc.IdentType`
        """

        code = "\n".join(
            [
                # Actual types do not matter, just need the typedefs for parsing
                "typedef unsigned char uint8_t;"
                "typedef unsigned short uint16_t;"
                "typedef unsigned int uint32_t;"
                "typedef unsigned long long uint64_t;"
                f"{typedecl} varname;"
            ]
        )
        parser = c_parser.CParser()
        ast = parser.parse(code)

        typemask = 0

        for node in ast:
            if not isinstance(node, c_ast.Decl):
                continue

            if node.name != "varname":
                continue

            nodetype = node.type
            if isinstance(nodetype, c_ast.PtrDecl):
                typemask |= IdentType.PTR_BIT.value

            quals: List[str] = []
            while not isinstance(nodetype, c_ast.IdentifierType):
                if isinstance(nodetype, c_ast.TypeDecl):
                    quals = nodetype.quals
                nodetype = nodetype.type

            typemask |= IdentType.from_fundamental_type(" ".join(nodetype.names)).value
            qualmask = 0
            for qual in quals:
                qualmask |= getattr(Qualifier, qual.upper()).value

            break

        ident = IdentType(typemask)

        return cls(ident=ident, qualifiers=qualmask)


class Origin(enum.IntEnum):
    """RPC origin"""

    HOST = 1  # Master
    REMOTE = 2  # Slave

    @property
    def peer(self) -> Origin:
        """Get peer of self"""
        return Origin((not (self.value - 1)) + 1)


class Direction(enum.IntEnum):
    """Parameter direction"""

    IN = 1
    OUT = 2

    INOUT = IN | OUT


@dataclasses.dataclass(slots=True)
class Parameter:
    """RPC parameter abstraction"""

    name: str
    typedecl: TypeDecl
    description: str
    size: str
    param_id: int
    direction: int
    is_size_parameter: bool = False
    is_fixed_size_string: bool = False

    def __post_init__(self) -> None:
        if self.typedecl.ident == IdentType.VOID:
            return

        self.description = self.description.replace("\n", "\n *" + 4 * " ")

        if self.typedecl.is_str:
            # Allow fixed-size strings
            if not self.size:
                self.size = f"strlen({self.name}) + 1u"

                if not (self.direction & Direction.IN.value):
                    raise InvalidDirectionError(
                        f"Terminated string parameter {self.name} cannot be used as out-only parameter"
                    )
            else:
                self.is_fixed_size_string = True

        elif not self.typedecl.is_ptr:
            if self.size:
                warnings.warn(f"Explicit size for parameter {self.name} discarded")
            self.size = f"sizeof({self.name})"

            # User explicitly setting direction out for integral parameter is an error
            if self.direction & Direction.OUT.value:
                raise InvalidDirectionError(
                    f"Integral parameter {self.name} cannot be used as out parameter"
                )

        if self.typedecl.is_const_qualified and (self.direction & Direction.OUT.value):
            raise InvalidDirectionError(
                "Cannot use const-qualified parameter as output"
            )

    @classproperty
    def VOID(cls) -> Parameter:
        return Parameter(
            name="",
            typedecl=TypeDecl(
                ident=IdentType.VOID,
                qualifiers=0,
            ),
            description="",
            size="",
            param_id=-1,
            direction=0,
        )

    @property
    def bytesize(self) -> str:
        """Get size of parameter, in bytes"""
        if not self.size:
            raise UnsizedFieldError(f"{self.name} is not sized")

        if (
            self.typedecl.is_ptr
            and not self.typedecl.is_str
            and (typesize := int(self.typedecl.ident.fundamental_type_size)) > 1
        ):
            return f"{self.size} * {typesize}"

        return self.size

    @property
    def is_in_parameter(self) -> bool:
        """Determine whether or not the parameter is an out parameter"""
        return not not (self.direction & Direction.IN.value)

    @property
    def is_out_parameter(self) -> bool:
        """Determine whether or not the parameter is an out parameter"""
        return not not (self.direction & Direction.OUT.value)

    @property
    def is_void(self) -> bool:
        """Check if the parameter is a void parameter"""
        return self == self.VOID

    @property
    def c_decl(self) -> str:
        """Get C declaration for the parameter.

        E.g. `uint8_t const *a` or `uint32_t sz`.
        """
        if self.is_void:
            return "void"
        return f"{self.typedecl.c_decl}{self.name}"

    @classmethod
    def from_yaml(cls, index: int, yml: Dict[str, Any]) -> Parameter:
        """Parse parameter from yaml portion.

        :param yml: Parameter portion of the core configuration file.
        :type yml: :class:`typing.Dict[str, typing.Any]`

        :return: The parsed parameter.
        :rtype: :class:`zrpc.rpc.Parameter`
        """
        typedecl = TypeDecl.from_yaml(yml["type"])

        default_dir = "inout"
        if not typedecl.is_ptr or typedecl.is_const_qualified:
            default_dir = "in"

        direction = getattr(Direction, yml.get("direction", default_dir).upper())

        return cls(
            name=yml["name"],
            typedecl=typedecl,
            description=yml.get("description", ""),
            size=yml.get("size", ""),
            param_id=index,
            direction=direction.value,
        )


@dataclasses.dataclass(slots=True)
class Rpc:
    """RPC abstraction"""

    name: str
    brief: str
    description: str
    origin: int
    parameters: List[Parameter]
    rpc_id: int
    crc: str
    want_reply: bool
    want_user_data: bool
    depends_on: str

    def __post_init__(self) -> None:
        self.description = self.description.replace("\n", "\n * ")

        if (match := re.match("^(0x)?[0-9a-fA-F]{1,2}$", self.crc)) is None:
            raise InvalidHexDigest(f"{self.crc} is not a valid 8 bit CRC")

        if match.group(1) is None:
            self.crc = f"0x{self.crc}"

    @property
    def depends_on_ifdef_expr(self) -> str:
        """Get #if expression suitable for disabling conditional RPCs

        :return: A chain of preprocesser conditionals to be passed to #if
        :rtype: str
        """
        if not self.depends_on:
            return "1"

        fd, path = tempfile.mkstemp()
        try:
            os.close(fd)

            with open(path, "w", encoding="ascii") as handle:
                handle.write(
                    'config A\n\tbool "Dummy"\n\tdepends on ' + self.depends_on + "\n"
                )

            cfg = kconfiglib.Kconfig(path)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)

        deps = cfg.syms["A"].direct_dep
        return self._resolve_dep_expr(deps)

    @classmethod
    def _resolve_dep_expr(
        cls,
        deps: Union[
            kconfiglib.Symbol,
            Tuple[int, kconfiglib.Symbol],
            Tuple[int, kconfiglib.Symbol, Any],
        ],
    ) -> str:
        """Recursively resolve parsed depends_on expression.

        :param deps: Recursively defined, one-or-two clause conditional in prefix notation
        :type deps: :class:`typing.Union[kconfiglib.Symbol, typing.Tuple[int, kconfiglib.Symbol], typing.Tuple[int, kconfiglib.Symbol, typing.Any]]`

        :return: The dependency expression as an #if conditional expression
        :rtype: str
        """

        if isinstance(deps, kconfiglib.Symbol):
            return f"defined(CONFIG_{deps.name})"

        if len(deps) == 2:
            assert deps[0] == kconfiglib.NOT
            return f"!{cls._resolve_dep_expr(deps[1])}"

        ops = {kconfiglib.AND: "&&", kconfiglib.OR: "||"}

        assert len(deps) == 3, f"{deps} is not a binary prefix expression"
        return f"({cls._resolve_dep_expr(deps[1])} {ops[deps[0]]} {cls._resolve_dep_expr(deps[2])})"  # type: ignore[misc]

    @property
    def non_void_parameters(self) -> List[Parameter]:
        """Get list of non-void parameters"""
        return [p for p in self.parameters if not p.is_void]

    @property
    def in_parameters(self) -> List[Parameter]:
        """Get list of (non-void) in-parameters"""
        return [p for p in self.parameters if p.is_in_parameter]

    @property
    def out_parameters(self) -> List[Parameter]:
        """Get list of (non-void) out-parameters"""
        if not self.want_reply:
            return []
        return [p for p in self.parameters if p.is_out_parameter]

    @property
    def size_parameters(self) -> List[Parameter]:
        """Get list of size parameters"""
        return [p for p in self.parameters if p.is_size_parameter]

    @classmethod
    def _validate_parameters(cls, parameters: List[Parameter]) -> None:
        """Validate parsed parameters.

        :param parameters: List of parsed RPC parameters.
        :type parameters: :class:`typing.List[zrpc.rpc.Parameter]`

        :raises MissingSizeFieldError: A pointer parameter without a set size field encountered.
        :raises InvalidSizeFieldError: The size field of a parameter references a non-existent parameter.
        """
        param_names = [p.name for p in parameters]
        by_name = {param.name: param for param in parameters}
        for param in parameters:
            if not param.size:
                raise MissingSizeFieldError(
                    f"Parameter {param.name} has no size field set"
                )

            if param.typedecl.is_ptr and param.size not in param_names:
                raise InvalidSizeFieldError(
                    f"Size field of {param.name} references non-existent parameter {param.size}"
                )

            if param.size == param.name:
                raise SelfRefError(f"Size field of {param.name} references itself")

            if param.size in by_name and by_name[param.size].typedecl.is_ptr:
                raise InvalidSizeFieldError(
                    f"Size field '{param.size}' is a pointer, should be integral"
                )

            if param.size in by_name:
                by_name[param.size].is_size_parameter = True

        cnt = collections.Counter(param_names)
        if any([v > 1 for v in cnt.values()]):
            raise DuplicationError(
                "The following parameter names occur more than once: "
                + json.dumps([k for k, v in cnt.items() if v > 1], indent=2)
            )

    @classmethod
    def from_yaml(cls, index: int, yml: Dict[str, Any]) -> Rpc:
        """Parse RPC from yaml portion.

        :param index: Index in the yaml list.
        :type index: int
        :param yml: Single RPC portion of the core configuration yaml file.
        :type yml: :class:`typing.Dict[str, typing.Any]`

        :return: The parsed RPC.
        :rtype: :class:`zrpc.rpc.Rpc`
        """
        origin_mask = 0
        for origin in yml["origin"]:
            origin_mask |= getattr(Origin, origin.upper())

        if not origin_mask:
            raise MissingOriginError(f"No origin specified for RPC {yml['name']}")

        parameters = [
            Parameter.from_yaml(i, p) for i, p in enumerate(yml.get("parameters", []))
        ]
        if parameters:
            try:
                cls._validate_parameters(parameters)
            except DuplicationError as exc:
                raise DuplicationError(
                    f"{yml['name']}: {str(exc)[:1].lower() + str(exc)[1:]}"
                )
        else:
            parameters = [Parameter.VOID]

        crc = crc8.crc8()
        crc.update(yaml.dump(yml).encode("utf-8"))

        return cls(
            name=yml["name"],
            brief=yml.get("brief", ""),
            description=yml.get("description", ""),
            origin=origin_mask,
            parameters=parameters,
            rpc_id=index,
            crc=crc.hexdigest(),
            want_reply=yml.get("want_reply", True),
            want_user_data=yml.get("want_user_data", False),
            depends_on=yml.get("depends_on", ""),
        )


@dataclasses.dataclass(slots=True)
class Channel:
    """RPC channel representation"""

    name: str
    chid: int
    rpcs: List[Rpc]
    description: str

    def __post_init__(self) -> None:
        self.description = self.description.replace("\n", "\n * ")

    @property
    def rpcs_from_host(self) -> List[Rpc]:
        """Obtain list of RPCs sent from host to remote"""
        return [rpc for rpc in self.rpcs if rpc.origin & Origin.HOST.value]

    @property
    def rpcs_to_host(self) -> List[Rpc]:
        """Obtain list of RPCs sent from remote to host"""
        return [rpc for rpc in self.rpcs if rpc.origin & Origin.REMOTE.value]

    @property
    def rpcs_from_remote(self) -> List[Rpc]:
        """Obtain list of RPCs sent from remote to host"""
        return self.rpcs_to_host

    @property
    def rpcs_to_remote(self) -> List[Rpc]:
        """Obtain list of RPCs sent from host to remote"""
        return self.rpcs_from_host

    @classmethod
    def from_yaml(cls, yml: Dict[str, Any]) -> Channel:
        """Parse channel from yaml portion.

        :param yml: Single channel portion of the core configuration yaml file.
        :type yml: :class:`typing.Dict[str, typing.Any]`

        :return: The parsed channel.
        :rtype: :class:`zrpc.rpc.Channel`
        """
        return cls(
            name=yml["name"],
            chid=int(yml["id"]),
            rpcs=[Rpc.from_yaml(i, rpc) for i, rpc in enumerate(yml["rpcs"])],
            description=yml.get("description", ""),
        )


@dataclasses.dataclass(slots=True)
class Specification:
    """zRPC specification"""

    channels: List[Channel]

    def __post_init__(self) -> None:
        cnt = collections.Counter([c.chid for c in self.channels])
        try:
            assert not any([v > 1 for v in cnt.values()])
        except AssertionError as exc:
            raise ChannelIdReuseError(
                f"Channel identifiers {', '.join([str(k) for k, v in cnt.items() if v > 1])} appear more than once"
            ) from exc

    @classmethod
    def from_yaml(cls, yml: Dict[str, Any]) -> Specification:
        """Generate specification from loaded yaml.

        :param yml: The loaded yaml dict.
        :type yml: :class:`typing.Dict[str, typing.Any]`

        :return: The parsed specification.
        :rtype: :class:`zrpc.rpc.Specification`
        """
        frame = inspect.currentframe()
        assert frame is not None
        p = (
            (pathlib.Path(inspect.getfile(frame)) / "..").resolve()
            / "schemas"
            / "core.yaml"
        )
        if not p.exists():
            raise FileNotFoundError(f"Found no core schema at {p}")

        with open(str(p), "r", encoding="utf-8") as handle:
            schema = yaml.safe_load(handle)

        pykwalify.core.Core(source_data=yml, schema_data=schema).validate()

        return cls(channels=[Channel.from_yaml(ch) for ch in yml["channels"]])

    @classmethod
    def from_file(cls, yaml_file: str | pathlib.Path) -> Specification:
        """Parse and validate zRPC specification from file.

        :param yaml_file: Path to the core configuration yaml file.
        :type yaml_file: :class:`typing.Union[str, pathlib.Path]`

        :raises FileNotFoundError: THe provided path does not refer to a file.

        :return: The parsed specification.
        :rtype: :class:`zrpc.rpc.Specification`
        """
        if isinstance(yaml_file, str):
            yaml_file = pathlib.Path(yaml_file)
        yaml_file = yaml_file.resolve()

        if not yaml_file.is_file():
            raise FileNotFoundError(f"{yaml_file} is not a regular file")

        with open(str(yaml_file), "r", encoding="utf-8") as handle:
            y = yaml.safe_load(handle)

        return cls.from_yaml(y)
