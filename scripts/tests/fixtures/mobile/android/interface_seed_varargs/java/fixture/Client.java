package fixture;
public class Client {public static <E extends CharSequence> E call(E text,String message,Object[] arguments)throws IllegalArgumentException {return Provider.select(text,message,arguments);}}
