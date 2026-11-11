import fixture.Core;
import fixture.Matcher;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class IterableCallbackForwarderHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned iterable callback forwarder mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object element=new Object(),marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[5];
        Iterator cursor=new Iterator() {
            public boolean hasNext() {counts[2]++;check(counts[2]==1 && Trace.stage==1,"hasNext order");if(mode==6)throw runtime;if(mode==7)throw error;return mode!=8;}
            public Object next() {counts[3]++;check(counts[2]==1 && counts[3]==1 && Trace.stage==1,"next order");if(mode==9)throw runtime;if(mode==10)throw error;return element;}
        };
        Collection actual=new AbstractCollection() {
            public int size(){return 1;}
            public Iterator iterator(){counts[1]++;check(counts[1]==1 && Trace.stage==1,"iterator order");return cursor;}
        };
        Collection input=mode==2?null:actual,late=new ArrayList();Iterable only=() -> cursor;
        Matcher matcher=mode==17?null:value -> {
            counts[4]++;check(counts[4]==1 && value==element && Trace.stage==1 && counts[3]==1,"exact callback element and order");
            if(mode==11)throw runtime;if(mode==12)throw error;return mode!=19;
        };
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.matcher=null;Trace.result=input;Trace.marker=marker;
        Trace.action=mode==3?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input && Trace.matcher==matcher,"exact provider references and order");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode!=1){Iterator it=input.iterator();if(it.hasNext())matcher.accept(it.next());}
            if(mode==13)Trace.result=late;if(mode==14)Trace.result=only;if(mode==15)Trace.result=new Object();
            if(mode==16)Trace.result=null;if(mode==18)Trace.marker=lateMarker;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input,matcher);break;
                case 1:result=Core.checked(input,matcher);break;
                case 2:result=Core.ignored(input,matcher);break;
                case 3:result=Core.relay(input,matcher);break;
                case 4:result=Core.narrow(input,matcher);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int has=mode==1 || mode==2 || mode==3 || mode==4 || mode==5?0:1;
        int next=has==0 || mode==6 || mode==7 || mode==8?0:1;
        int callbacks=next==0 || mode==9 || mode==10 || mode==17?0:1;
        boolean actionFailed=mode==2 || mode==3 || mode==4 || mode==5 || mode==6 || mode==7 || mode==9 || mode==10 || mode==11 || mode==12 || mode==17;
        check(Trace.calls==1 && Trace.input==input && Trace.matcher==matcher && Trace.stage==(actionFailed?1:2)
                && counts[0]==(mode==3?0:1) && counts[1]==has && counts[2]==has && counts[3]==next && counts[4]==callbacks,"exact original partial effects");
        if(mode==4 || mode==6 || mode==9 || mode==11)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7 || mode==10 || mode==12)check(failure==error,"error identity");
        else if(mode==2 || mode==3 || mode==17)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==15 || mode==14 && (route==1 || route==4))check(failure instanceof ClassCastException,"original provider or delayed Collection cast");
        else check(failure==null && result==(route==2?mode==18?lateMarker:marker:mode==13?late:mode==14?only:mode==16?null:input),"opaque returned identity and late marker");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,iterable-callback-forwarder=matched");
    }
}
