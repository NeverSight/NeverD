package fixture;public final class Trace {
 public static int initializations,failAt;public static long events;public static float observed;public static Object token,saved,partial,after;public static RuntimeException failure;
 public static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
 public static Object box(float input){if(initializations!=1)throw failure;observed=input;hit(1);return token;}
}
