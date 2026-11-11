package fixture;
public final class Trace {
 public static long events;
 public static int failStep,failOccurrence,matching,buffers,indexes,index,nextIndex;
 public static Throwable failure;
 public static Object[] current,alternate,initialized;
 public static Object receiver;
 public static Target partial;
 public static void hit(int step) {
  events=events*10+step;
  if(step==failStep && ++matching==failOccurrence) {
   if(failure instanceof Error) throw (Error)failure;
   throw (RuntimeException)failure;
  }
 }
}
