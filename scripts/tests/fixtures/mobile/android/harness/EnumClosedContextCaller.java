import fixture.Client;
public final class EnumClosedContextCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum call(Class type,String text){return Client.call(type,text);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum lookup(Class type,String text){return Client.lookup(type,text);}
}
