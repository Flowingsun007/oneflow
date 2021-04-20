"""
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""
import oneflow as flow

from typing import Optional, Sequence, Sized, Union, List, Tuple
import collections
from oneflow.python.oneflow_export import oneflow_export
from oneflow.python.nn.module import Module
from oneflow.python.nn.modules.utils import (
    _single,
    _pair,
    _triple,
    _reverse_repeat_tuple,
)
from oneflow.python.nn.common_types import _size_1_t, _size_2_t, _size_3_t
from oneflow.python.ops.nn_ops import calc_pool_padding, get_dhw_offset
import oneflow.python.framework.id_util as id_util
from oneflow.python.framework.tensor import register_tensor_op_by_module
from oneflow.python.framework.tensor import register_op_by_module


@oneflow_export("Transpose")
@register_tensor_op_by_module("tmp.transpose")
@register_op_by_module("tmp.transpose")
class Transpose(Module):
    r"""
    """

    def __init__(
        self,
        perm: Sequence[int] = None,
        conjugate: bool = False,
        batch_axis_non_change: bool = False,
        name: Optional[str] = None,
    ) -> None:
        super().__init__()

        assert isinstance(perm, (tuple, list))

        if conjugate:
            raise NotImplementedError

        self._op = (
            flow.builtin_op("transpose", name)
            .Input("input")
            .Output("output")
            .Attr("perm", perm)
            .Build()
        )

    def forward(self, x):
        return self._op(x)[0]


@oneflow_export("ExpandDims")
@register_tensor_op_by_module("tmp.expand_dims")
@register_op_by_module("tmp.expand_dims")
class ExpandDims(Module):
    r"""
    """

    def __init__(self, axis: int, name: Optional[str] = None,) -> None:
        super().__init__()

        self._op = (
            flow.builtin_op("expand_dims", name)
            .Input("in")
            .Output("out")
            .Attr("axis", axis)
            .Build()
        )

    def forward(self, x):
        return self._op(x)[0]


@oneflow_export("Squeeze")
@register_tensor_op_by_module("tmp.squeeze")
@register_op_by_module("tmp.squeeze")
class Squeeze(Module):
    r"""
    """

    def __init__(
        self, axis: Optional[Sequence[int]] = None, name: Optional[str] = None,
    ) -> None:
        super().__init__()

        self._op = (
            flow.builtin_op("squeeze", name)
            .Input("in")
            .Output("out")
            .Attr("axes", axis)
            .Build()
        )

    def forward(self, x):
        return self._op(x)[0]
