package fixture;
public class Trace {
    public static long events;
    public static int actions,elements,delimiters,failAt;
    public static Object[] values;
    public static Object element,payload;
    public static CharSequence delimiter;
    public static String prefix,suffix,text,delimiterText,result;
    public static RuntimeException failure;
    public static Runnable action;
    public static void hit(int event){events=events*10+event;if(event==failAt)throw failure;}
}
