import fixture.Client;
public final class EnumTokenDefaultCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum defaults(Class type,Object value) {return Client.defaults(type,value);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum explicit(Class type,Object value,Enum fallback) {return Client.explicit(type,value,fallback);}
}
