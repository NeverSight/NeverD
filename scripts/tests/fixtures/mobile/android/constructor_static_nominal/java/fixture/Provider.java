package fixture;
public class Provider {public static String wrap(Object value){int n=++Trace.factories;if(n==1)Trace.inputLeft=value;else Trace.inputRight=value;Trace.hit(n);return n==1?Trace.left:Trace.right;}}
