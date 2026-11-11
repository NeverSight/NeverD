import fixture.Client;
import fixture.Source;
import fixture.View;
public final class EnumInterfaceKeyCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum call(View view,Class type,Object key){return Client.call(view,type,key);}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Enum fallback(Source source,Class type,Object key,Enum fallback){return Client.fallback(source,type,key,fallback);}
}
