import fixture.*;
import java.io.IOException;
import java.util.Map;
import java.util.Comparator;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ComparablePairFactoryHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned comparable pair factory");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    @SuppressWarnings("rawtypes") private static Comparator call(int route,int operation)throws Throwable {
        if(route==0)return operation==0?Client.call():Client.full();
        if(route==1)return operation==0?ComparablePairFactoryCaller.call():ComparablePairFactoryCaller.full();
        if(route==2){try{return (Comparator)Client.class.getMethod(operation==0?"call":"full").invoke(null);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Comparator)MethodHandles.publicLookup().findStatic(Client.class,operation==0?"call":"full",MethodType.methodType(Comparator.class)).invokeExact();
    }
    @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new Failure("owned stage");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),comparePayload=new Object();
            class Value implements Comparable<Value>{final int id;Value(int id){this.id=id;}public int compareTo(Value other){Trace.compares++;require(id==1&&(mode==2?other==null:other!=null&&other.id==2)&&Trace.events==16234);Trace.hit(5);Trace.comparePayload=comparePayload;if(mode==11)ComparablePairFactoryHarness.<RuntimeException>fail(checked);return -37;}}
            final Value leftValue=new Value(1),rightValue=new Value(2);
            final Map.Entry left=mode==1?null:new Map.Entry(){public Object getKey(){require(false);return null;}public Object setValue(Object value){require(false);return null;}public Object getValue(){Trace.leftReads++;require(Trace.events==162);Trace.hit(3);if(mode==8)throw runtime;return mode==9?new Object():mode==10?null:leftValue;}};
            final Map.Entry right=new Map.Entry(){public Object getKey(){require(false);return null;}public Object setValue(Object value){require(false);return null;}public Object getValue(){Trace.rightReads++;require(Trace.events==1623);Trace.hit(4);return mode==2?null:rightValue;}};
            final Comparator selected=new Comparator(){public int compare(Object a,Object b){require(a==left&&b==right);return ((Comparable)((Map.Entry)a).getValue()).compareTo(((Map.Entry)b).getValue());}};
            Trace.events=0;Trace.path=0;Trace.leftReads=0;Trace.rightReads=0;Trace.compares=0;Trace.payload=sentinel;Trace.comparePayload=sentinel;Trace.result=sentinel;Trace.platform=null;
            final int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.path==operation+1);Trace.hit(6);Trace.payload=payload;Trace.result=mode==3?null:selected;if(mode==4)throw runtime;if(mode==5)throw error;}};
            Comparator result=null;Integer compared=null;Throwable failure=null;try{result=call(route,operation);compared=Integer.valueOf(result.compare(left,right));}catch(Throwable caught){failure=caught;}
            boolean actionReached=mode!=6,factoryReturned=mode!=4&&mode!=5&&mode!=6&&mode!=7,leftRead=factoryReturned&&mode!=1&&!(mode==3&&operation==0),rightRead=leftRead&&mode!=8&&mode!=9,comparedValue=rightRead&&mode!=10;
            long events=mode==6?1:mode==4||mode==5?16:!leftRead?162:!rightRead?1623:!comparedValue?16234:162345;
            require(Trace.calls==calls+1&&Trace.path==operation+1&&Trace.events==events&&Trace.leftReads==(leftRead?1:0)&&Trace.rightReads==(rightRead?1:0)&&Trace.compares==(comparedValue?1:0));
            require(Trace.payload==(actionReached?payload:sentinel)&&Trace.result==(actionReached?(mode==3?null:selected):sentinel)&&Trace.comparePayload==(comparedValue?comparePayload:sentinel));
            if(!factoryReturned)require(result==null&&Trace.platform==null);else if(operation==0)require(result==(mode==3?null:selected)&&Trace.platform==null);else require(result==Trace.platform&&result!=null);
            Throwable expected=mode==4||mode==8?runtime:mode==5?error:mode==6||mode==7?Trace.failure:mode==11?checked:null;
            if(expected!=null)require(failure==expected&&compared==null);
            else if(mode==1||mode==10||(mode==3&&operation==0))require(failure instanceof NullPointerException&&compared==null);
            else if(mode==9)require(failure instanceof ClassCastException&&compared==null);
            else require(failure==null&&compared.intValue()==-37);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
