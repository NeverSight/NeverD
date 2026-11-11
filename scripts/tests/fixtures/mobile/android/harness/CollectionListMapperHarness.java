import fixture.Core;
import fixture.Classifier;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class CollectionListMapperHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned collection list mapper mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object element=new Object(),marker=new Object(),lateMarker=new Object();String leaf=new String("leaf"),lateLeaf=new String("late");
        List inner=new ArrayList(),lateInner=new ArrayList(),original=new ArrayList(),late=new ArrayList(),linked=new LinkedList(),empty=new ArrayList(),badInner=new ArrayList(),emptyInner=new ArrayList(),badLeaf=new ArrayList(),nullLeaf=new ArrayList();
        inner.add(leaf);lateInner.add(lateLeaf);original.add(inner);late.add(lateInner);linked.add(inner);badInner.add(new Object());emptyInner.add(new ArrayList());List wrong=new ArrayList(),nullable=new ArrayList();wrong.add(new Object());nullable.add(null);badLeaf.add(wrong);nullLeaf.add(nullable);
        Object chosen=mode==14?linked:mode==20?empty:mode==21?badInner:mode==22?emptyInner:mode==23?badLeaf:mode==24?nullLeaf:original;
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[5];
        Iterator cursor=new Iterator() {
            public boolean hasNext(){counts[2]++;check(counts[2]==1 && Trace.stage==1,"hasNext order");if(mode==6)throw runtime;if(mode==7)throw error;return mode!=8;}
            public Object next(){counts[3]++;check(counts[2]==1 && counts[3]==1 && Trace.stage==1,"next order");if(mode==9)throw runtime;if(mode==10)throw error;return element;}
        };
        Collection actual=new AbstractCollection() {
            public int size(){return 1;}
            public Iterator iterator(){counts[1]++;check(counts[1]==1 && Trace.stage==1,"iterator order");return cursor;}
        };
        Collection input=mode==2?null:actual;
        Classifier classifier=mode==17?null:value -> {counts[4]++;check(counts[4]==1 && value==element && Trace.stage==1 && counts[3]==1,"exact classifier element and order");if(mode==11)throw runtime;if(mode==12)throw error;return mode==19?Integer.MIN_VALUE:7;};
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.classifier=null;Trace.result=chosen;Trace.marker=marker;
        Trace.action=mode==3?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input && Trace.classifier==classifier,"exact provider references and order");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode!=1){Iterator it=input.iterator();if(it.hasNext())classifier.key(it.next());}
            if(mode==13)Trace.result=late;if(mode==15)Trace.result=new Object();if(mode==16)Trace.result=null;if(mode==18)Trace.marker=lateMarker;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input,classifier);break;
                case 1:result=Core.checked(input,classifier);break;
                case 2:result=Core.ignored(input,classifier);break;
                case 3:result=Core.relay(input,classifier);break;
                case 4:result=Core.nested(input,classifier);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int has=mode==1 || mode==2 || mode==3 || mode==4 || mode==5?0:1;
        int next=has==0 || mode==6 || mode==7 || mode==8?0:1;
        int callbacks=next==0 || mode==9 || mode==10 || mode==17?0:1;
        boolean actionFailed=mode==2 || mode==3 || mode==4 || mode==5 || mode==6 || mode==7 || mode==9 || mode==10 || mode==11 || mode==12 || mode==17;
        check(Trace.calls==1 && Trace.input==input && Trace.classifier==classifier && Trace.stage==(actionFailed?1:2) && counts[0]==(mode==3?0:1) && counts[1]==has && counts[2]==has && counts[3]==next && counts[4]==callbacks,"exact partial provider effects");
        if(mode==4 || mode==6 || mode==9 || mode==11)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7 || mode==10 || mode==12)check(failure==error,"error identity");
        else if(mode==2 || mode==3 || mode==17 || mode==16 && route==4)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==15 || mode==14 && route==1 || route==4 && (mode==21 || mode==23))check(failure instanceof ClassCastException,"original provider or nested cast");
        else if(route==4 && (mode==20 || mode==22))check(failure instanceof IndexOutOfBoundsException,"original nested index failure");
        else {
            Object outer=mode==13?late:mode==16?null:chosen;
            check(failure==null && result==(route==2?mode==18?lateMarker:marker:route==4?mode==24?null:mode==13?lateLeaf:leaf:outer),"opaque outer/inner references and late marker");
            if(route!=2 && route!=4 && outer!=null)check(result==outer,"original outer list identity");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,collection-list-mapper=matched");
    }
}
