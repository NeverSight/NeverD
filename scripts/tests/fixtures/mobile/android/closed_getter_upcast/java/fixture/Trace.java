package fixture;
public final class Trace {
 public static long events;
 public static int failStep;
 public static Throwable failure;
 public static Object replacementFirst,replacementAgain,replacementMap,seen,firstReceiver,mappingReceiver;
 public static void hit(int step) {
  events=events*10+step;
  if(step==failStep) {
   if(failure instanceof Error) throw (Error)failure;
   throw (RuntimeException)failure;
  }
 }
}
