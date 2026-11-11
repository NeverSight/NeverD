package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class ArrayElementSelectorHarness {
    private static void check(boolean value) {
        if(!value)throw new AssertionError("owned array element selector mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value)throws T {throw (T)value;}
    private static final class Observer extends Trace {
        final int route,mode,mask;
        final Object input,rule;
        final Object[] values;
        final Object replacement,later,independent;
        final Throwable marker;
        final boolean wanted;
        Object first,result;
        int stage,ranks,flags,selected;
        boolean failed;
        Observer(int route,int mode,int mask,Object input,Object rule,Object[] values,
                 Object replacement,Object later,Object independent,Throwable marker) {
            this.route=route;this.mode=mode;this.mask=mask;this.input=input;this.rule=rule;
            this.values=values;this.replacement=replacement;this.later=later;
            this.independent=independent;this.marker=marker;wanted=Trace.want;
        }
        void fail(int desired) {if(mode==desired){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Object array,int mask,Object callback,Object value) {
            check(array==input && mask==this.mask && callback==rule);
            if(phase==6){check(stage==0 && value==null);stage=1;fail(10);return;}
            if(phase==1){
                check(stage==1 && value==null);stage=2;
                if(mode==17)values[Trace.index]=replacement;
                if(mode==24)Trace.want=!wanted;
                fail(11);return;
            }
            if(phase==2){
                check(stage==2 && value==values[Trace.index]);first=value;stage=3;
                if(mode==18)values[Trace.index]=replacement;
                fail(12);return;
            }
            if(phase==3){
                check(stage==3 && ranks++==0 && value==first);stage=4;
                if(mode==19)Trace.rank=2;
                fail(13);return;
            }
            if(phase==4){
                check(stage==4 && flags++==0 && value==first);stage=5;
                if(mode==20)Trace.flag=false;
                fail(14);return;
            }
            if(phase==5){
                check(stage==5);
                selected=Trace.rank+(Trace.flag==wanted ? 0 : Trace.delta);
                Object expected=Trace.override ? independent : values[selected];
                check(value==expected);result=value;stage=6;
                if(mode==21){values[selected]=later;Trace.reply=later;}
                fail(15);return;
            }
            check(phase==7 && stage==6 && value==(route==2 ? null : result));stage=7;
            fail(16);
        }
        void finish(Object returned,Throwable problem) {
            if(mode==3 || mode==4)check(problem instanceof ClassCastException && stage==1);
            else if(mode==5)check(problem instanceof NullPointerException && stage==2);
            else if(mode==6)check(problem instanceof NullPointerException && stage==3);
            else if(mode==7 || mode==8)check(problem instanceof ArrayIndexOutOfBoundsException && stage==2);
            else if(mode==9)check(problem instanceof ArrayIndexOutOfBoundsException && stage==5 && ranks==1 && flags==1);
            else if(mode>=10 && mode<=16)check(failed && problem==marker && returned==null && stage==mode-9);
            else if(mode==23 && route!=2)check(problem instanceof ClassCastException && stage==7 && returned==null);
            else check(problem==null && stage==7 && returned==(route==2 ? null : result));
            if(stage>=4)check(ranks==1);
            if(stage>=5)check(flags==1);
            if(mode==17 || mode==18)check(values[Trace.index]==replacement);
            if(mode==21)check(values[selected]==later && Trace.reply==later && result!=later);
            if(mode==24)check(Trace.want!=wanted);
        }
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String first=new String("first"),second=new String("second"),third=new String("third");
            Object[] values=attempt==0 ? new Object[]{first,second,third} : new String[]{first,second,third};
            Object input=values,rule=new Core.Checker();
            String replacement=new String("replacement"),later=new String("later"),independent=new String("independent");
            if(mode==2)values[1]=null;
            if(mode==3)input=new Object();
            if(mode==4)rule=new Object();
            if(mode==5)input=null;
            if(mode==6)rule=null;
            if(mode==7){values=new Object[0];input=values;}
            int mask=0x731+attempt;
            Trace.array=input;Trace.mode=mask;Trace.index=mode==1 ? 2 : mode==8 ? -1 : 0;
            Trace.rank=mode==1 ? 0 : mode==9 ? 99 : 1;
            Trace.flag=mode!=1;Trace.want=true;Trace.delta=1;
            Trace.override=mode==22 || mode==23;
            Object reply=mode==23 ? new Object() : independent;Trace.reply=reply;
            Throwable marker=(mode&1)==0 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,mask,input,rule,values,replacement,later,reply,marker);
            Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Client.run(input,mask,rule);
                else if(route==1)result=MethodHandles.lookup().findStatic(Client.class,"run",
                    MethodType.methodType(String.class,Object.class,int.class,Object.class)).invokeWithArguments(input,mask,rule);
                else Client.ignore(input,mask,rule);
            }catch(Throwable failure){problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,array-element-selector=matched");
    }
}
