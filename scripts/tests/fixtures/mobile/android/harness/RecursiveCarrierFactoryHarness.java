import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.Comparator;
public final class RecursiveCarrierFactoryHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned recursive carrier factory");}
    private static int probeCalls;
    private static Probe seenReceiver;
    private static Object seenArgument;
    private static Throwable platformFailure;
    private static final class Probe implements Comparable<Probe> {
        final int value;Probe(int value){this.value=value;}
        public int compareTo(Probe other){probeCalls++;seenReceiver=this;seenArgument=other;if(platformFailure instanceof RuntimeException)throw (RuntimeException)platformFailure;if(platformFailure instanceof Error)throw (Error)platformFailure;if(other==null)throw new NullPointerException("owned compare null");return Integer.compare(value,other.value);}
    }
    @SuppressWarnings("rawtypes")
    private static Comparator call(int route,int operation)throws Throwable {
        if(route==0)return operation==0?Client.call():Client.lookup();
        if(route==1)return operation==0?RecursiveCarrierFactoryCaller.call():RecursiveCarrierFactoryCaller.lookup();String name=operation==0?"call":"lookup";
        if(route==2){try{return (Comparator)Client.class.getMethod(name).invoke(null);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Comparator)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Comparator.class)).invokeExact();
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure"),compareFailure=new IllegalArgumentException("owned compare failure");final Error actionError=new Error("owned action error"),compareError=new Error("owned compare error");
        if(operation==1){
            Probe a=new Probe(mode==10?Integer.MIN_VALUE:2),b=mode==7?a:new Probe(mode==10?Integer.MAX_VALUE:mode==6?2:9);
            final Object left=mode==1||mode==3?null:mode==8?Integer.valueOf(7):a,right=mode==2||mode==3?null:mode==8?"owned mismatch":mode==9?new Object():b;
            platformFailure=mode==4||mode==11?compareFailure:mode==5?compareError:null;
            for(int attempt=0;attempt<2;attempt++){
                int calls=Trace.calls;Trace.events=0;probeCalls=0;seenReceiver=null;seenArgument=null;Comparator actual=null;int value=42;Throwable failure=null;
                try{actual=call(route,operation);value=actual.compare(left,right);}catch(Throwable error){failure=error;}
                require(actual!=null&&Trace.calls==calls&&Trace.events==0);
                boolean entered=mode!=2&&mode!=3&&mode!=8&&mode!=9;require(probeCalls==(entered?1:0));if(entered)require(seenReceiver==right&&seenArgument==left);
                if(platformFailure!=null)require(failure==platformFailure&&value==42);
                else if(mode==1||mode==2||mode==3)require(failure instanceof NullPointerException&&value==42);
                else if(mode==8||mode==9)require(failure instanceof ClassCastException&&value==42);
                else require(failure==null&&value==Integer.compare(((Probe)right).value,((Probe)left).value));
                System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+probeCalls);
            }
            return;
        }
        final Object left=mode==1?null:new Object(),right=mode==2?null:new Object();
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        final Comparator sentinelComparator=new Comparator(){public int compare(Object a,Object b){throw new AssertionError("owned recursive carrier factory");}};
        for(int attempt=0;attempt<2;attempt++){
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.payload=sentinel;Trace.left=sentinel;Trace.right=sentinel;Trace.result=sentinelComparator;int calls=Trace.calls,comparisons=Trace.comparisons;
            final Comparator desired=mode==9?null:new Comparator(){public int compare(Object a,Object b){require(a==left&&b==right&&Trace.payload==payload);Trace.comparisons++;Trace.left=a;Trace.right=b;Trace.hit(4);if(mode==10)throw compareFailure;if(mode==11)throw compareError;return -1000-mode;}};
            Trace.action=mode==3?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.result==sentinelComparator&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Comparator actual=sentinelComparator;int value=42;Throwable failure=null;try{actual=call(route,operation);value=actual.compare(left,right);}catch(Throwable error){failure=error;}
            boolean returned=mode!=3&&mode!=4&&mode!=5&&mode!=6&&mode!=7,compared=returned&&mode!=9;
            require(Trace.calls==calls+1&&Trace.comparisons==comparisons+(compared?1:0)&&Trace.events==(mode==3||mode==6?1:mode==4||mode==5?13:compared?1324:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==10?compareFailure:mode==11?compareError:null;
            if(expected!=null)require(failure==expected&&value==42);else if(mode==3||mode==9)require(failure instanceof NullPointerException&&value==42);else require(failure==null&&value==-1000-mode);
            require(actual==(returned?desired:sentinelComparator)&&Trace.payload==(mode==3||mode==6?sentinel:payload)&&Trace.left==(compared?left:sentinel)&&Trace.right==(compared?right:sentinel));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
