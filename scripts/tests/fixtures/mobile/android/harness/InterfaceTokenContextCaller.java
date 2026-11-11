import fixture.Client;
import java.io.File;
import java.io.IOException;
public final class InterfaceTokenContextCaller {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static CharSequence call(File context,Class type,CharSequence[] data)throws IOException{return Client.call(context,type,data);}
}
