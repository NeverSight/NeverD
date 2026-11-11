package fixture;
import java.io.File;
import java.io.IOException;
public class Client {public static <T extends CharSequence> T call(File context,Class<T> type,CharSequence... data)throws IOException {return Provider.read(context,type,data);}}
