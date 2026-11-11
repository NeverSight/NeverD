import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked","deprecation"})
public final class IndexedListTransferHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned indexed list transfer mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object element=new Object(),marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[3];
        int index=mode==15?Integer.MIN_VALUE:mode==16?Integer.MAX_VALUE:mode==17?-1:mode==18?0:7;
        ArrayList actual=new ArrayList() {
            public int size(){counts[1]++;check(counts[1]==1 && Trace.stage==1,"size order");if(mode==5)throw runtime;if(mode==6)throw error;return mode==12?0:1;}
            public Object get(int position){counts[2]++;check(counts[2]==1 && position==0 && counts[1]==1 && Trace.stage==1,"get order and index");if(mode==7)throw runtime;if(mode==8)throw error;return element;}
        };
        ArrayList input=mode==1?null:actual;
        Trace.calls=Trace.stage=0;Trace.index=42;Trace.input=null;Trace.result=new ArrayList();Trace.marker=marker;Trace.built=Trace.inner=null;
        Trace.action=mode==2?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input && Trace.index==index,"exact input/scalar and provider order");
            if(mode==3)throw runtime;if(mode==4)throw error;
            ArrayList inner=new ArrayList(),outer=new ArrayList();if(input.size()!=0){inner.add(input.get(0));outer.add(inner);if(mode==19)outer.add(inner);}
            Trace.inner=inner;Trace.built=outer;Trace.result=outer;
            if(mode==9)Trace.result=null;if(mode==10)Trace.result=new Object();if(mode==11){Trace.result=new LinkedList(outer);Trace.built=Trace.result;}
            if(mode==13){ArrayList late=new ArrayList(outer);Trace.result=late;Trace.built=late;}
            if(mode==14)Trace.marker=lateMarker;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input,index);break;
                case 1:result=Core.checked(input,index);break;
                case 2:result=Core.ignored(input,index);break;
                case 3:result=Core.relay(input,index);break;
                case 4:result=Core.old(input,index);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int action=mode==2?0:1;
        int sizes=action==0 || mode==1 || mode==3 || mode==4?0:1;
        int gets=sizes==0 || mode==5 || mode==6 || mode==12?0:1;
        boolean actionFailed=mode>=1 && mode<=8;
        check(Trace.calls==1 && Trace.input==input && Trace.index==index && Trace.stage==(actionFailed?1:2)
                && counts[0]==action && counts[1]==sizes && counts[2]==gets,"exact original partial effects");
        if(mode==1 || mode==2)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==3 || mode==5 || mode==7)check(failure==runtime,"runtime identity");
        else if(mode==4 || mode==6 || mode==8)check(failure==error,"error identity");
        else if(mode==10 || mode==11 && route==1)check(failure instanceof ClassCastException,"original provider or delayed ArrayList cast");
        else check(failure==null && result==(route==2?mode==14?lateMarker:marker:mode==9?null:Trace.built),"opaque result or late marker identity");
        if(!actionFailed) {
            List built=(List)Trace.built;check(built.size()==(mode==12?0:mode==19?2:1),"original outer size");
            if(mode!=12)check(built.get(0)==Trace.inner && ((List)Trace.inner).get(0)==element && (mode!=19 || built.get(1)==Trace.inner),"nested element identity and aliases");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,indexed-list-transfer=matched");
    }
}
