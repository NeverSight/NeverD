package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class TailLoopFinalHarness {
    private static void check(boolean value){if(!value)throw new AssertionError("owned tail loop final mismatch");}
    private static final class Failure extends Exception{}
    private static final class Fault extends Error{}
    @SuppressWarnings("unchecked") private static <T extends Throwable> RuntimeException unchecked(Throwable value)throws T{throw (T)value;}
    private static final class Observer extends Trace {
        final int mode;
        final Token[] source;
        final Token replacement;
        final Token[] later;
        final String changed;
        final Throwable marker;
        Core subject;
        Token[] result;
        String resultText;
        int stage,clones,steps;
        boolean failed;
        Token pending;
        String contents="";
        Observer(int mode,Token[] source,Token replacement,Token[] later,String changed,Throwable marker){
            this.mode=mode;this.source=source;this.replacement=replacement;this.later=later;this.changed=changed;this.marker=marker;
        }
        void fail(int wanted){if(mode==wanted){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Core self,Token[] input,Token[] copy,int index,Object value){
            check(input==source && Trace.partial==self);
            if(phase==0){
                check(stage==0 && copy==null && index==-1 && value==null);subject=self;stage=1;
                check(self.getValues()==null && self.getText()==null);
                if(mode==23)source[1]=replacement;
                fail(7);return;
            }
            check(self==subject);
            if(phase==3){
                check(stage==2 && self.getValues()==result && self.getText()==null && copy==null);
                check(index==clones && value==source[index]);pending=(Token)value;clones++;
                if(mode==16 && index==0)source[0]=replacement;
                if(mode==24 && index==1){failed=true;throw unchecked(marker);}
                fail(8);return;
            }
            if(phase==5){
                check(stage==2 && self.getValues()==result && self.getText()==null && index==steps && clones==steps+1);
                Token made=(Token)value;
                check(made!=null && copy[index]==made);
                check(Trace.overrideClone ? made==Trace.cloneReply : made!=pending && made.id==pending.id);
                contents+=made.id+"|";check(Trace.builder.toString().equals(contents));
                if(result==null)result=copy;check(copy==result);steps++;
                if(mode==17 && index==0)source[1]=replacement;
                fail(9);return;
            }
            if(phase==1){
                check(stage==1 && index==-1 && value==null && self.getText()==null);
                if(source.length==0){check(clones==0 && steps==0 && copy==Trace.empty);result=copy;}
                else {check(clones==0 && steps==0 && copy!=null && copy.length==source.length);result=copy;}
                check(self.getValues()==result);stage=2;
                if(mode==21)Trace.emptyText=changed;
                if(mode==22)Trace.empty=later;
                fail(10);return;
            }
            if(phase==4){
                check(stage==2 && self.getValues()==result && self.getText()==null && copy==result && index==-1 && clones==source.length && steps==source.length);
                check(value==Trace.builder && value.toString().equals(contents));stage=3;
                if(mode==18)result[0]=replacement;
                if(mode==19)Trace.textReply=changed;
                fail(11);return;
            }
            check(phase==2 && stage==(source.length==0 ? 2 : 3) && index==-1 && copy==result);
            resultText=source.length==0 ? Trace.emptyText : Trace.textReply;
            check(value==resultText && self.getValues()==result && self.getText()==resultText);stage=4;
            if(mode==20)Trace.textReply=changed;
            fail(12);
        }
        void finish(Core resultObject,Throwable problem){
            if(mode==4){check(problem instanceof ClassCastException && subject==null && Trace.partial==null);return;}
            check(subject==Trace.partial);
            if(mode==3)check(problem instanceof NullPointerException && stage==1 && subject.getValues()==null && subject.getText()==null);
            else if(mode==5 || mode==6 || mode==14)check(problem instanceof NullPointerException && stage==2 && subject.getValues()==result && subject.getText()==null);
            else if(mode==13)check(problem instanceof ClassCastException && stage==2 && subject.getValues()==result && subject.getText()==null);
            else if((mode>=7 && mode<=12) || mode==24)check(failed && problem==marker && resultObject==null);
            else check(problem==null && resultObject==subject && stage==4);
            if(stage>=2)check(subject.getValues()==result);
            if(stage==4)check(subject.getText()==resultText);
            if(mode==16)check(source[0]==replacement);
            if(mode==17 || mode==23)check(source[1]==replacement);
            if(mode==18)check(result[0]==replacement);
            if(mode==22)check(Trace.empty==later && result!=later);
        }
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);check(route>=0 && route<3 && mode>=0 && mode<25);
        check(Core.class.getConstructor(Token[].class).isVarArgs());
        check(java.lang.reflect.Modifier.isFinal(Core.class.getDeclaredField("values").getModifiers()));
        for(int attempt=0;attempt<2;++attempt){
            boolean empty=mode<=2 || mode==21 || mode==22;
            Token[] source=empty ? new Token[0] : new Token[]{new Token(11+attempt),new Token(23+attempt)};
            Token replacement=new Token(47+attempt),override=new Token(59+attempt);
            if(mode==5)source[0]=null;
            if(mode==6)source[1]=null;
            if(mode==3)source=null;
            Object input=mode==4 ? new Object() : source;
            Trace.partial=null;Trace.input=source;Trace.empty=mode==1 ? null : new Token[0];
            Trace.emptyText=mode==2 ? null : new String("empty");Trace.textReply=new String("reply");
            Trace.overrideClone=mode>=13 && mode<=15;Trace.cloneReply=mode==13 ? new Object() : mode==14 ? null : override;
            Trace.builder=null;Trace.index=-1;
            Throwable marker=(mode&1)==0 ? new Fault() : new Failure();
            Observer observer=new Observer(mode,source,replacement,new Token[0],new String("changed"),marker);Trace.hook=observer;
            Core result=null;Throwable problem=null;
            try{
                if(route==0)result=new Core((Token[])input);
                else if(route==1)result=(Core)Core.class.getConstructor(Token[].class).newInstance((Object)(Token[])input);
                else result=(Core)MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,Token[].class)).asFixedArity().invokeWithArguments(input);
            }catch(InvocationTargetException failure){problem=failure.getCause();}catch(Throwable failure){problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,tail-loop-final=matched");
    }
}
