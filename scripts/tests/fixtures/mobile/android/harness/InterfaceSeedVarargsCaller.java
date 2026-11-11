import fixture.Client;
public final class InterfaceSeedVarargsCaller {
    public static CharSequence call(CharSequence text,String message,Object[] arguments){return Client.call(text,message,arguments);}
}
