import fixture.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class BinaryClosedContextCaller {
    public static Output call(Input input,Mapper context){return Client.call(input,context);}
    public static Output reversed(Mapper context,Input input){return Client.reversed(context,input);}
}
