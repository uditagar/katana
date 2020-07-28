from llvmlite import ir


def call_raw_function_pointer(func_ptr, function_type, args, c):
    function_type = ir.FunctionType(ir.PointerType(ir.IntType(8)),
                                    (ir.PointerType(ir.IntType(8)),))
    ptr = ir.Constant(ir.IntType(64), func_ptr).inttoptr(ir.PointerType(function_type))
    # HACK: Add a field to ptr which is expected by builder.call based on the
    #  assumption that the function is a normal Function.
    ptr.function_type = function_type
    return c.builder.call(ptr, args)


def interpret_numba_wrapper_tables(tables, globals=None):
    from galois.numba.wrappers import SimpleNumbaPointerWrapper
    for typ, table in tables:
        assert hasattr(typ, "address") and hasattr(typ.address, "__get__") and not hasattr(typ.address, "__call__"), \
            "{}.address does not exist or is not a property.".format(typ)
        Type = SimpleNumbaPointerWrapper(typ)
        interpret_numba_wrapper_table(Type, table)
        if globals:
            globals[typ.__name__ + "_numba_wrapper"] = Type
            globals[typ.__name__ + "_numba_type"] = Type.type


def interpret_numba_wrapper_table(Type, table):
    for name, func_type, impl_func_name, addr in table:
        Type.register_method(name, func_type, impl_func_name, addr=addr)