package fixture;
import java.nio.ByteOrder;
public class Client {public static <N extends Number> N call(byte[] data,Class<N> type,ByteOrder order)throws IllegalArgumentException {return Provider.read(data,type,order);}}
