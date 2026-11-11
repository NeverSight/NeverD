import fixture.*;
import java.util.Map;
import java.util.Comparator;
import java.util.HashMap;
import java.util.TreeMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PairedKeyConsumerHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned paired key consumer");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static TreeMap call(int route,int operation,Map source,Comparator comparator)throws Throwable {
        if(route==0)return operation==0?Client.call(source):Client.full(source,comparator);
        if(route==1)return operation==0?PairedKeyConsumerCaller.call(source):PairedKeyConsumerCaller.full(source,comparator);
        if(route==2){try{return (TreeMap)(operation==0?Client.class.getMethod("call",Map.class).invoke(null,source):Client.class.getMethod("full",Map.class,Comparator.class).invoke(null,source,comparator));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (TreeMap)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(TreeMap.class,Map.class)).invokeExact(source);
        return (TreeMap)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(TreeMap.class,Map.class,Comparator.class)).invokeExact(source,comparator);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");Trace.failure=new Failure("owned stage");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Map source=mode==1?null:new HashMap();final Object first=mode==2?null:new Object(),second=new Object(),inserted=new Object(),laterKey=new Object(),laterValue=new Object(),sentinel=new Object(),payload=new Object(),comparePayload=new Object(),earlyResult=new Object();
            final TreeMap desired=new TreeMap();final Object desiredValue=mode==11?Integer.valueOf(19):new String("owned result");desired.put("a",desiredValue);
            final Object returned=mode==9?new HashMap():mode==10?null:desired;
            final boolean hasComparator=operation==1&&mode!=3;
            Trace.events=0;Trace.order=101;Trace.source=null;Trace.comparator=null;Trace.first=first;Trace.second=second;Trace.inserted=inserted;Trace.payload=sentinel;Trace.comparePayload=sentinel;Trace.result=sentinel;
            final int calls=Trace.calls,comparisons=Trace.comparisons,actions=Trace.actions;
            final Comparator comparator=hasComparator?new Comparator(){public int compare(Object left,Object right){Trace.comparisons++;require(left==first&&right==second&&Trace.events==1&&Trace.source==source&&Trace.comparator==this);Trace.hit(6);Trace.comparePayload=comparePayload;Trace.first=laterKey;Trace.inserted=laterValue;Trace.result=earlyResult;Trace.order=91;if(mode==4)throw runtime;if(mode==5)throw error;return -37;}}:null;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==(hasComparator?1623:123)&&Trace.source==source&&Trace.comparator==comparator&&Trace.order==(hasComparator?-37:0)&&source.size()==1&&source.get(hasComparator?laterKey:first)==(hasComparator?laterValue:inserted));Trace.hit(7);Trace.payload=payload;Trace.result=returned;source.put("callback",payload);Trace.first=payload;if(mode==4||mode==8)throw runtime;if(mode==5)throw error;}};
            TreeMap result=null;String actual="initial";Integer length=null;Throwable failure=null;
            try{result=call(route,operation,source,comparator);actual=(String)result.get("a");length=Integer.valueOf(actual.length());}catch(Throwable caught){failure=caught;}
            boolean compared=hasComparator&&mode!=6,compareFailed=compared&&(mode==4||mode==5),stored=mode!=1&&mode!=6&&mode!=7&&!compareFailed,actionReached=stored;
            long events=mode==6?1:compareFailed?16:mode==1||mode==7?(hasComparator?162:12):mode==4||mode==5||mode==8?(hasComparator?16237:1237):(hasComparator?162374:12374);
            require(Trace.calls==calls+1&&Trace.comparisons==comparisons+(compared?1:0)&&Trace.actions==actions+(actionReached?1:0)&&Trace.events==events&&Trace.source==source&&Trace.comparator==comparator);
            require(Trace.order==(mode==6?101:compareFailed?91:hasComparator?-37:0)&&Trace.comparePayload==(compared?comparePayload:sentinel)&&Trace.payload==(actionReached?payload:sentinel));
            require(Trace.first==(actionReached?payload:compared?laterKey:first)&&Trace.inserted==(compared?laterValue:inserted)&&Trace.second==second&&Trace.result==(actionReached?returned:compared?earlyResult:sentinel));
            if(source!=null){require(source.size()==(stored?1:0)+(actionReached?1:0));if(stored)require(source.get(hasComparator?laterKey:first)==(hasComparator?laterValue:inserted));if(actionReached)require(source.get("callback")==payload);}
            Throwable expected=mode==4||mode==8?runtime:mode==5?error:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&result==null&&actual.equals("initial")&&length==null);
            else if(mode==1||mode==10)require(failure instanceof NullPointerException&&result==null&&actual.equals("initial")&&length==null);
            else if(mode==9)require(failure instanceof ClassCastException&&result==null&&actual.equals("initial")&&length==null);
            else if(mode==11)require(failure instanceof ClassCastException&&result==desired&&actual.equals("initial")&&length==null);
            else require(failure==null&&result==desired&&actual==desiredValue&&length.intValue()==actual.length());
            require(desired.size()==1&&desired.get("a")==desiredValue);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
