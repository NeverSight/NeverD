import fixture.Core;
import java.io.IOException;
import java.nio.file.FileVisitResult;
public final class NominalSuperResultCaller {
    public static FileVisitResult submit(Core receiver,String value,IOException error)throws IOException{return receiver.submit(value,error);}
    public static FileVisitResult checked(Core receiver,Object value,IOException error)throws IOException{return receiver.checked(value,error);}
}
