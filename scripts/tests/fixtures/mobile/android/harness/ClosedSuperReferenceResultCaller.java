import fixture.Core;
import java.io.IOException;
import java.util.ArrayList;
public final class ClosedSuperReferenceResultCaller {
    public static ArrayList<String> submit(Core receiver,String value,ArrayList<String> items,IOException error)throws IOException{return receiver.submit(value,items,error);}
    public static ArrayList<String> checked(Core receiver,Object value,Object items,IOException error)throws IOException{return receiver.checked(value,items,error);}
    public static void discard(Core receiver,String value,ArrayList<String> items,IOException error)throws IOException{receiver.discard(value,items,error);}
}
