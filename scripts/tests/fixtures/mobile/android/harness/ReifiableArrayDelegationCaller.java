import fixture.Core;
import java.io.File;
public final class ReifiableArrayDelegationCaller {public static Core file(File source,Class<?>[] data){return new Core(source,data);}public static Core text(String source,Class<?>[] data){return new Core(source,data);}}
