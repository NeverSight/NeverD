import fixture.Client;
import fixture.Child;
import java.io.File;
import java.util.Collection;
public final class ArrayOverloadSeparationCaller {public static Child call(boolean array,String[] data,Collection<File> values){return array?Client.call(data,values):Client.fresh(values);}}
