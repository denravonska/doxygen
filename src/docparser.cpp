/******************************************************************************
 *
 * 
 *
 *
 * Copyright (C) 1997-2002 by Dimitri van Heesch.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation under the terms of the GNU General Public License is hereby 
 * granted. No representations are made about the suitability of this software 
 * for any purpose. It is provided "as is" without express or implied warranty.
 * See the GNU General Public License for more details.
 *
 * Documents produced by Doxygen are derivative works derived from the
 * input used in their production; they are not affected by this license.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <qfile.h>
#include <qfileinfo.h>
#include <qcstring.h>
#include <qstack.h>
#include <qdict.h>

#include "doxygen.h"
#include "debug.h"

#include "docparser.h"
#include "doctokenizer.h"
#include "cmdmapper.h"
#include "printdocvisitor.h"

#define DBG(x) do {} while(0)
//#define DBG(x) printf x

//---------------------------------------------------------------------------

static QStack<DocNode> g_nodeStack;
static QStack<DocStyleChange> g_styleStack;

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a preformatted node */
static bool insidePRE(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlPre) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a html list item node */
static bool insideLI(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlListItem) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a unordered html list node */
static bool insideUL(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlList && 
        ((DocHtmlList *)n)->type()==DocHtmlList::Unordered) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a ordered html list node */
static bool insideOL(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_HtmlList && 
        ((DocHtmlList *)n)->type()==DocHtmlList::Ordered) return TRUE;
    n=n->parent();
  }
  return FALSE;
}

//---------------------------------------------------------------------------

/*! Returns TRUE iff node n is a child of a language node */
static bool insideLang(DocNode *n)
{
  while (n)
  {
    if (n->kind()==DocNode::Kind_Language) return TRUE;
    n=n->parent();
  }
  return FALSE;
}


//---------------------------------------------------------------------------

// forward declaration
static bool defaultHandleToken(DocNode *parent,int tok, 
                               QList<DocNode> &children,bool
                               handleWord=TRUE);


static int handleStyleArgument(DocNode *parent,QList<DocNode> &children,
                               const QCString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
	cmdName.data(),doctokenizerYYlineno);
    return tok;
  }
  while ((tok=doctokenizerYYlex()) && tok!=TK_WHITESPACE && tok!=TK_NEWPARA)
  {
    if (!defaultHandleToken(parent,tok,children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
	  printf("Error: Illegal command \\%s as the argument of a \\%s command at line %d\n",
	       g_token->name.data(),cmdName.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  return tok==TK_NEWPARA ? TK_NEWPARA : RetVal_OK;
}

static void handleStyleEnter(DocNode *parent,QList<DocNode> &children,DocStyleChange::Style s)
{
  DBG(("HandleStyleEnter\n"));
  DocStyleChange *sc= new DocStyleChange(parent,g_nodeStack.count(),s,TRUE);
  children.append(sc);
  g_styleStack.push(sc);
}

static void handleStyleLeave(DocNode *parent,QList<DocNode> &children,DocStyleChange::Style s,const char *tagName)
{
  DBG(("HandleStyleLeave\n"));
  if (g_styleStack.isEmpty() ||                           // no style change
      g_styleStack.top()->style()!=s ||                   // wrong style change
      g_styleStack.top()->position()!=g_nodeStack.count() // wrong position
     )
  {
    printf("Error: found </%s> tag at line %d without matching <%s> in the same paragraph\n",
        tagName,doctokenizerYYlineno,tagName);
  }
  else // end the section
  {
    DocStyleChange *sc= new DocStyleChange(parent,g_nodeStack.count(),s,FALSE);
    children.append(sc);
    g_styleStack.pop();
  }
}

static void handlePendingStyleCommands(DocNode *parent,QList<DocNode> &children)
{
  if (!g_styleStack.isEmpty())
  {
    DocStyleChange *sc = g_styleStack.top();
    while (sc && sc->position()>=g_nodeStack.count()) 
    { // there are unclosed style modifiers in the paragraph
      const char *cmd;
      switch (sc->style())
      {
        case DocStyleChange::Bold:        cmd = "b"; break;
        case DocStyleChange::Italic:      cmd = "em"; break;
        case DocStyleChange::Code:        cmd = "code"; break;
        case DocStyleChange::Center:      cmd = "center"; break;
        case DocStyleChange::Small:       cmd = "small"; break;
        case DocStyleChange::Subscript:   cmd = "subscript"; break;
        case DocStyleChange::Superscript: cmd = "superscript"; break;
      }
      printf("Error: end of paragraph at line %d without end of style "
             "command </%s>\n",doctokenizerYYlineno,cmd);
      children.append(new DocStyleChange(parent,g_nodeStack.count(),sc->style(),FALSE));
      g_styleStack.pop();
      sc = g_styleStack.top();
    }
  }
}

/* Helper function that deals with the most common tokens allowed in
 * title like sections. 
 * @param parent     Parent node, owner of the children list passed as 
 *                   the third argument. 
 * @param tok        The token to process.
 * @param children   The list of child nodes to which the node representing
 *                   the token can be added.
 * @param handleWord Indicates if word token should be processed
 * @retval TRUE      The token was handled.
 * @retval FALSE     The token was not handled.
 */
static bool defaultHandleToken(DocNode *parent,int tok, QList<DocNode> &children,bool
    handleWord)
{
  DBG(("token %s at %d",tokToString(tok),doctokenizerYYlineno));
  if (tok==TK_WORD || tok==TK_SYMBOL || tok==TK_URL || 
      tok==TK_COMMAND || tok==TK_HTMLTAG
     )
  {
    DBG((" name=%s",g_token->name.data()));
  }
  DBG(("\n"));
  QCString tokenName = g_token->name;
  switch (tok)
  {
    case TK_COMMAND: 
      switch (CmdMapper::map(tokenName))
      {
        case CMD_BSLASH:
          children.append(new DocSymbol(parent,DocSymbol::BSlash));
          break;
        case CMD_AT:
          children.append(new DocSymbol(parent,DocSymbol::At));
          break;
        case CMD_LESS:
          children.append(new DocSymbol(parent,DocSymbol::Less));
          break;
        case CMD_GREATER:
          children.append(new DocSymbol(parent,DocSymbol::Greater));
          break;
        case CMD_AMP:
          children.append(new DocSymbol(parent,DocSymbol::Amp));
          break;
        case CMD_DOLLAR:
          children.append(new DocSymbol(parent,DocSymbol::Dollar));
          break;
        case CMD_HASH:
          children.append(new DocSymbol(parent,DocSymbol::Hash));
          break;
        case CMD_PERCENT:
          children.append(new DocSymbol(parent,DocSymbol::Percent));
          break;
        case CMD_EMPHASIS:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Italic,TRUE));
            int retval=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Italic,FALSE));
            if (retval==TK_NEWPARA) goto handlepara;
          }
          break;
        case CMD_BOLD:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Bold,TRUE));
            int retval=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Bold,FALSE));
            if (retval==TK_NEWPARA) goto handlepara;
          }
          break;
        case CMD_CODE:
          {
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Code,TRUE));
            int retval=handleStyleArgument(parent,children,tokenName);
            children.append(new DocStyleChange(parent,g_nodeStack.count(),DocStyleChange::Code,FALSE));
            if (retval==TK_NEWPARA) goto handlepara;
          }
          break;
        case CMD_HTMLONLY:
          {
            doctokenizerYYsetStateHtmlOnly();
            int retval = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_token->verb,DocVerbatim::HtmlOnly));
            if (retval==0) printf("Error: htmlonly section ended without end marker at line %d\n",
                doctokenizerYYlineno);
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_LATEXONLY:
          {
            doctokenizerYYsetStateLatexOnly();
            int retval = doctokenizerYYlex();
            children.append(new DocVerbatim(parent,g_token->verb,DocVerbatim::LatexOnly));
            if (retval==0) printf("Error: latexonly section ended without end marker at line %d\n",
                doctokenizerYYlineno);
            doctokenizerYYsetStatePara();
          }
          break;
        case CMD_FORMULA:
          {
            DocFormula *form=new DocFormula(parent,g_token->id);
            children.append(form);
          }
          break;
        default:
          return FALSE;
      }
      break;
    case TK_HTMLTAG:
      {
        switch (HtmlTagMapper::map(tokenName))
        {
          case HTML_BOLD:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Bold);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Bold,tokenName);
            }
            break;
          case HTML_CODE:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Code);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Code,tokenName);
            }
            break;
          case HTML_EMPHASIS:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Italic);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Italic,tokenName);
            }
            break;
          case HTML_SUB:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Subscript);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Subscript,tokenName);
            }
            break;
          case HTML_SUP:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Superscript);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Superscript,tokenName);
            }
            break;
          case HTML_CENTER:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Center);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Center,tokenName);
            }
            break;
          case HTML_SMALL:
            if (!g_token->endTag)
            {
              handleStyleEnter(parent,children,DocStyleChange::Small);
            }
            else
            {
              handleStyleLeave(parent,children,DocStyleChange::Small,tokenName);
            }
            break;
          default:
            return FALSE;
            break;
        }
      }
      break;
    case TK_SYMBOL: 
      {
        char letter='\0';
        DocSymbol::SymType s = DocSymbol::decodeSymbol(tokenName,&letter);
        if (s!=DocSymbol::Unknown)
        {
          children.append(new DocSymbol(parent,s,letter));
        }
        else
        {
          return FALSE;
        }
      }
      break;
    case TK_WHITESPACE: 
    case TK_NEWPARA: 
handlepara:
      if (insidePRE(parent) || !children.isEmpty())
      {
        children.append(new DocWhiteSpace(parent,g_token->chars));
      }
      break;
    case TK_WORD: 
      if (handleWord)
        children.append(new DocWord(parent,g_token->name));
      else
        return FALSE;
      break;
    case TK_URL:
      children.append(new DocURL(parent,g_token->name));
      break;
    default:
      return FALSE;
  }
  return TRUE;
}


//---------------------------------------------------------------------------

DocSymbol::SymType DocSymbol::decodeSymbol(const QCString &symName,char *letter)
{
  int l=symName.length();
  DBG(("decodeSymbol(%s) l=%d\n",symName.data(),l));
  if      (symName=="&copy;")  return DocSymbol::Copy;
  else if (symName=="&lt;")    return DocSymbol::Less;
  else if (symName=="&gt;")    return DocSymbol::Greater;
  else if (symName=="&amp;")   return DocSymbol::Amp;
  else if (symName=="&apos;")  return DocSymbol::Apos;
  else if (symName=="&quot;")  return DocSymbol::Quot;
  else if (symName=="&szlig;") return DocSymbol::Szlig;
  else if (symName=="&nbsp;")  return DocSymbol::Nbsp;
  else if (l==6 && symName.right(4)=="uml;")  
  {
    *letter=symName.at(1);
    return DocSymbol::Uml;
  }
  else if (l==8 && symName.right(6)=="acute;")  
  {
    *letter=symName.at(1);
    return DocSymbol::Acute;
  }
  else if (l==8 && symName.right(6)=="grave;")
  {
    *letter=symName.at(1);
    return DocSymbol::Grave;
  }
  else if (l==7 && symName.right(5)=="circ;")
  {
    *letter=symName.at(1);
    return DocSymbol::Circ;
  }
  else if (l==8 && symName.right(6)=="tilde;")
  {
    *letter=symName.at(1);
    return DocSymbol::Tilde;
  }
  else if (l==8 && symName.right(6)=="cedil;")
  {
    *letter=symName.at(1);
    return DocSymbol::Cedil;
  }
  else if (l==7 && symName.right(5)=="ring;")
  {
    *letter=symName.at(1);
    return DocSymbol::Ring;
  }
  return DocSymbol::Unknown;
}

//---------------------------------------------------------------------------

int DocLanguage::parse()
{
  int retval;
  DBG(("DocLanguage::parse() start\n"));
  g_nodeStack.push(this);

  // parse one or more paragraphs
  do
  {
    DocPara *par = new DocPara(this);
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);

  DBG(("DocLanguage::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

void DocSecRefItem::parse()
{
  DBG(("DocSecRefItem::parse() start\n"));
  g_nodeStack.push(this);

  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a \\refitem at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();
  handlePendingStyleCommands(this,m_children);
  DBG(("DocSecRefItem::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

void DocSecRefList::parse()
{
  DBG(("DocSecRefList::parse() start\n"));
  g_nodeStack.push(this);

  int tok=doctokenizerYYlex();
  // skip white space
  while (tok==TK_WHITESPACE) tok=doctokenizerYYlex();
  // handle items
  while (tok)
  {
    if (tok==TK_COMMAND)
    {
      switch (CmdMapper::map(g_token->name))
      {
        case CMD_SECREFITEM:
          {
            int tok=doctokenizerYYlex();
            if (tok!=TK_WHITESPACE)
            {
              printf("Error: expected whitespace after \\refitem command at line %d\n",
                  doctokenizerYYlineno);
              break;
            }
            tok=doctokenizerYYlex();
            if (tok!=TK_WORD)
            {
              printf("Error: unexpected token %s as the argument of \\refitem at line %d.\n",
                  tokToString(tok),doctokenizerYYlineno);
              break;
            }

            DocSecRefItem *item = new DocSecRefItem(this,g_token->name);
            m_children.append(item);
            item->parse();
          }
          break;
        case CMD_ENDSECREFLIST:
          goto endsecreflist;
        default:
          printf("Error: Illegal command %s as part of a \\secreflist at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          goto endsecreflist;
      }
    }
    else
    {
      printf("Error: Unexpected token %s inside section reference list at line %d\n",
          tokToString(tok),doctokenizerYYlineno);
      goto endsecreflist;
    }
    tok=doctokenizerYYlex();
  }

endsecreflist:
  DBG(("DocSecRefList::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}



//---------------------------------------------------------------------------

void DocRef::parse()
{
  g_nodeStack.push(this);
  DBG(("DocRef::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a \\ref at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }

  handlePendingStyleCommands(this,m_children);
  DBG(("DocRef::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}

//---------------------------------------------------------------------------

QCString DocLink::parse(bool isJavaLink)
{
  QCString result;
  g_nodeStack.push(this);
  DBG(("DocLink::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children,FALSE))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          switch (CmdMapper::map(g_token->name))
          {
            case CMD_ENDLINK:
              if (isJavaLink)
              {
                printf("Error: {@link.. ended with @endlink command at line %d\n",
                    doctokenizerYYlineno);
              }
              goto endlink;
            default:
              printf("Error: Illegal command %s as part of a \\ref at line %d\n",
                  g_token->name.data(),doctokenizerYYlineno);
              break;
          }
          break;
        case TK_SYMBOL: 
          printf("Error: Unsupported symbol %s found at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_WORD: 
          if (isJavaLink) // special case to detect closing }
          {
            QCString w = g_token->name;
            uint l=w.length();
            int p;
            if (w=="}")
            {
              goto endlink;
            }
            else if ((p=w.find('}'))!=-1)
            {
              m_children.append(new DocWord(this,w.left(p)));
              if ((uint)p<l-1) // something left after the } (for instance a .)
              {
                result=w.right(l-p-1);
              }
              goto endlink;
            }
          }
          m_children.append(new DocWord(this,g_token->name));
          break;
        default:
          printf("Error: Unexpected token %s at line %d\n",
            g_token->name.data(),doctokenizerYYlineno);
        break;
      }
    }
  }
  if (tok==0)
  {
    printf("Error: Unexpected end of comment at line %d while inside"
           " link command\n",doctokenizerYYlineno); 
  }
endlink:

  handlePendingStyleCommands(this,m_children);
  DBG(("DocLink::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return result;
}


//---------------------------------------------------------------------------

void DocDotFile::parse()
{
  g_nodeStack.push(this);
  DBG(("DocDotFile::parse() start\n"));

  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a \\dotfile at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();

  handlePendingStyleCommands(this,m_children);
  DBG(("DocDotFile::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}


//---------------------------------------------------------------------------

void DocImage::parse()
{
  g_nodeStack.push(this);
  DBG(("DocImage::parse() start\n"));

  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a \\image at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();

  handlePendingStyleCommands(this,m_children);
  DBG(("DocImage::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
}


//---------------------------------------------------------------------------

int DocHtmlHeader::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlHeader::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a <h%d> tag at line %d\n",
	       g_token->name.data(),m_level,doctokenizerYYlineno);
          break;
        case TK_HTMLTAG:
          {
            int tagId=HtmlTagMapper::map(g_token->name);
            if (tagId==HTML_H1 && g_token->endTag) // found </h1> tag
            {
              if (m_level!=1)
              {
                printf("Error: <h%d> ended with </h1> at line %d\n",
                    m_level,doctokenizerYYlineno); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H2 && g_token->endTag) // found </h2> tag
            {
              if (m_level!=2)
              {
                printf("Error: <h%d> ended with </h2> at line %d\n",
                    m_level,doctokenizerYYlineno); 
              }
              goto endheader;
            }
            else if (tagId==HTML_H3 && g_token->endTag) // found </h3> tag
            {
              if (m_level!=3)
              {
                printf("Error: <h%d> ended with </h3> at line %d\n",
                    m_level,doctokenizerYYlineno); 
              }
              goto endheader;
            }
            else
            {
              printf("Error: Unexpected html tag <%s%s> found at line %d within <h%d> context\n",
                  g_token->endTag?"/":"",g_token->name.data(),doctokenizerYYlineno,m_level);
            }
          }
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok==0)
  {
    printf("Error: Unexpected end of comment at line %d while inside"
           " <h%d> tag\n",doctokenizerYYlineno,m_level); 
  }
endheader:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlHeader::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHRef::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHRef::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a <a>..</a> block at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_HTMLTAG:
          {
            int tagId=HtmlTagMapper::map(g_token->name);
            if (tagId==HTML_A && g_token->endTag) // found </a> tag
            {
              goto endhref;
            }
            else
            {
              printf("Error: Unexpected html tag <%s%s> found at line %d within <a href=...> context\n",
                  g_token->endTag?"/":"",g_token->name.data(),doctokenizerYYlineno);
            }
          }
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok==0)
  {
    printf("Error: Unexpected end of comment at line %d while inside"
           " <a href=...> tag\n",doctokenizerYYlineno); 
  }
endhref:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHRef::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocInternal::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocInternal::parse() start\n"));

  // first parse any number of paragraphs
  do
  {
    DocPara *par = new DocPara(this);
    retval=par->parse();
    if (!par->isEmpty()) m_children.append(par);
    if (retval==TK_LISTITEM)
    {
      printf("Error: Invalid list item found at line %d!\n",doctokenizerYYlineno);
    }
  } while (retval!=0 && retval!=RetVal_Section);

  // then parse any number of level1 sections
  while (retval==RetVal_Section)
  {
    SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
    int secLev = sec->type==SectionInfo::Subsection ? 2 : 1;
    if (secLev!=1) // wrong level
    {
      printf("Error: Expected level 1 section, found a section with level %d at line %d.\n",secLev,doctokenizerYYlineno);
      break;
    }
    else
    {
      DocSection *s=new DocSection(this,secLev,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }

  if (retval==RetVal_Internal)
  {
    printf("Error: \\internal command found inside internal section\n");
  }

  DBG(("DocInternal::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocIndexEntry::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocIndexEntry::parse() start\n"));
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after \\addindex command at line %d\n",
        doctokenizerYYlineno);
    goto endindexentry;
  }
  while ((tok=doctokenizerYYlex()) && tok!=TK_WHITESPACE && tok!=TK_NEWPARA)
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a \\addindex at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok!=TK_WHITESPACE) retval=tok;
endindexentry:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocIndexEntry::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlCaption::parse()
{
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlCaption::parse() start\n"));
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a <caption> tag at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
          printf("Error: Unsupported symbol %s found at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_HTMLTAG:
          {
            int tagId=HtmlTagMapper::map(g_token->name);
            if (tagId==HTML_CAPTION && g_token->endTag) // found </caption> tag
            {
              retval = RetVal_OK;
              goto endcaption;
            }
            else
            {
              printf("Error: Unexpected html tag <%s%s> found at line %d within <caption> context\n",
                  g_token->endTag?"/":"",g_token->name.data(),doctokenizerYYlineno);
            }
          }
          break;
        default:
          printf("Error: Unexpected token %s at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok==0)
  {
    printf("Error: Unexpected end of comment at line %d while inside"
           " <caption> tag\n",doctokenizerYYlineno); 
  }
endcaption:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlCaption::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlCell::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlCell::parse() start\n"));

  // parse one or more paragraphs
  do
  {
    DocPara *par = new DocPara(this);
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);

  DBG(("DocHtmlCell::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlRow::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlRow::parse() start\n"));

  bool isHeading=FALSE;
  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=HtmlTagMapper::map(g_token->name);
    if (tagId==HTML_TD && !g_token->endTag) // found <td> tag
    {
    }
    else if (tagId==HTML_TH && !g_token->endTag) // found <th> tag
    {
      isHeading=TRUE;
    }
    else // found some other tag
    {
      printf("Error: expected <td> or <th> tag at line %d but "
          "found <%s> instead!\n",doctokenizerYYlineno,g_token->name.data());
      goto endrow;
    }
  }
  else if (tok==0) // premature end of comment
  {
    printf("Error: unexpected end of comment at line %d while looking"
        " for a html description title\n",doctokenizerYYlineno);
    goto endrow;
  }
  else // token other than html token
  {
    printf("Error: expected <td> or <th> tag at line %d but found %s token instead!\n",
        doctokenizerYYlineno,tokToString(tok));
    goto endrow;
  }

  // parse one or more cells
  do
  {
    DocHtmlCell *td=new DocHtmlCell(this,isHeading);
    m_children.append(td);
    retval=td->parse();
    isHeading = retval==RetVal_TableHCell;
  }
  while (retval==RetVal_TableCell || retval==RetVal_TableHCell);

endrow:
  DBG(("DocHtmlRow::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlTable::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlTable::parse() start\n"));
  
getrow:
  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=HtmlTagMapper::map(g_token->name);
    if (tagId==HTML_TR && !g_token->endTag) // found <tr> tag
    {
      // no caption, just rows
      retval=RetVal_TableRow;
    }
    else if (tagId==HTML_CAPTION && !g_token->endTag) // found <caption> tag
    {
      if (m_caption)
      {
        printf("Error: table already has a caption, found another one at line %d\n",
            doctokenizerYYlineno);
      }
      else
      {
        m_caption = new DocHtmlCaption(this);
        retval=m_caption->parse();

        if (retval==RetVal_OK) // caption was parsed ok
        {
          goto getrow;
        }
      }
    }
    else // found wrong token
    {
      printf("Error: expected <tr> or <caption> tag at line %d but "
          "found <%s%s> instead!\n",doctokenizerYYlineno,
          g_token->endTag ? "/" : "", g_token->name.data());
    }
  }
  else if (tok==0) // premature end of comment
  {
      printf("Error: unexpected end of comment at line %d while looking"
          " for a <tr> or <caption> tag\n",doctokenizerYYlineno);
  }
  else // token other than html token
  {
    printf("Error: expected <tr> tag at line %d but found %s token instead!\n",
        doctokenizerYYlineno,tokToString(tok));
  }
       
  // parse one or more rows
  while (retval==RetVal_TableRow)
  {
    DocHtmlRow *tr=new DocHtmlRow(this);
    m_children.append(tr);
    retval=tr->parse();
  } 

  DBG(("DocHtmlTable::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval==RetVal_EndTable ? RetVal_OK : retval;
}

//---------------------------------------------------------------------------

int DocHtmlDescTitle::parse()
{
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescTitle::parse() start\n"));

  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a <dt> tag at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
          printf("Error: Unsupported symbol %s found at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_HTMLTAG:
          {
            int tagId=HtmlTagMapper::map(g_token->name);
            if (tagId==HTML_DD && !g_token->endTag) // found <dd> tag
            {
              retval = RetVal_DescData;
              goto endtitle;
            }
            else if (tagId==HTML_DT && g_token->endTag)
            {
              // ignore </dt> tag.
            }
            else
            {
              printf("Error: Unexpected html tag <%s%s> found at line %d within <dt> context\n",
                  g_token->endTag?"/":"",g_token->name.data(),doctokenizerYYlineno);
            }
          }
          break;
        default:
          printf("Error: Unexpected token %s at line %d\n",
              g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  if (tok==0)
  {
    printf("Error: Unexpected end of comment at line %d while inside"
        " <dt> tag\n",doctokenizerYYlineno); 
  }
endtitle:
  handlePendingStyleCommands(this,m_children);
  DBG(("DocHtmlDescTitle::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlDescData::parse()
{
  int retval=0;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescData::parse() start\n"));

  do
  {
    DocPara *par = new DocPara(this);
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);
  
  DBG(("DocHtmlDescData::parse() end\n"));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlDescList::parse()
{
  int retval=RetVal_OK;
  g_nodeStack.push(this);
  DBG(("DocHtmlDescList::parse() start\n"));

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE || tok==TK_NEWPARA) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=HtmlTagMapper::map(g_token->name);
    if (tagId==HTML_DT && !g_token->endTag) // found <dt> tag
    {
      // continue
    }
    else // found some other tag
    {
      printf("Error: expected <dt> tag at line %d but "
          "found <%s> instead!\n",doctokenizerYYlineno,g_token->name.data());
      goto enddesclist;
    }
  }
  else if (tok==0) // premature end of comment
  {
    printf("Error: unexpected end of comment at line %d while looking"
        " for a html description title\n",doctokenizerYYlineno);
    goto enddesclist;
  }
  else // token other than html token
  {
    printf("Error: expected <dt> tag at line %d but found %s token instead!\n",
        doctokenizerYYlineno,tokToString(tok));
    goto enddesclist;
  }

  do
  {
    DocHtmlDescTitle *dt=new DocHtmlDescTitle(this);
    m_children.append(dt);
    DocHtmlDescData *dd=new DocHtmlDescData(this);
    m_children.append(dd);
    retval=dt->parse();
    if (retval==RetVal_DescData)
    {
      retval=dd->parse();
    }
    else
    {
      // error
      break;
    }
  } while (retval==RetVal_DescTitle);

  if (retval==0)
  {
    printf("Error: unexpected end of comment at line %d while inside <dl> block\n",
        doctokenizerYYlineno);
  }

enddesclist:

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocHtmlDescList::parse() end\n"));
  return retval==RetVal_EndDesc ? RetVal_OK : retval;
}

//---------------------------------------------------------------------------

int DocHtmlPre::parse()
{
  int rv;
  g_nodeStack.push(this);

  do
  {
    DocPara *par = new DocPara(this);
    m_children.append(par);
    rv=par->parse();
  }
  while (rv==TK_NEWPARA);

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return rv==RetVal_EndPre ? RetVal_OK : rv;
}

//---------------------------------------------------------------------------

int DocHtmlListItem::parse()
{
  DBG(("DocHtmlListItem::parse() start\n"));
  int retval=0;
  g_nodeStack.push(this);

  // parse one or more paragraphs
  do
  {
    DocPara *par = new DocPara(this);
    m_children.append(par);
    retval=par->parse();
  }
  while (retval==TK_NEWPARA);

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocHtmlListItem::parse() end retval=%x\n",retval));
  return retval;
}

//---------------------------------------------------------------------------

int DocHtmlList::parse()
{
  DBG(("DocHtmlList::parse() start\n"));
  int retval=RetVal_OK;
  g_nodeStack.push(this);

  // get next token
  int tok=doctokenizerYYlex();
  // skip whitespace
  while (tok==TK_WHITESPACE) tok=doctokenizerYYlex();
  // should find a html tag now
  if (tok==TK_HTMLTAG)
  {
    int tagId=HtmlTagMapper::map(g_token->name);
    if (tagId==HTML_LI && !g_token->endTag) // found <li> tag
    {
      // ok, we can go on.
    }
    else // found some other tag
    {
      printf("Error: expected <li> tag at line %d but "
          "found <%s> instead!\n",doctokenizerYYlineno,g_token->name.data());
      goto endlist;
    }
  }
  else if (tok==0) // premature end of comment
  {
    printf("Error: unexpected end of comment at line %d while looking"
        " for a html list item\n",doctokenizerYYlineno);
    goto endlist;
  }
  else // token other than html token
  {
    printf("Error: expected <li> tag at line %d but found %s token instead!\n",
        doctokenizerYYlineno,tokToString(tok));
    goto endlist;
  }

  do
  {
    DocHtmlListItem *li=new DocHtmlListItem(this);
    m_children.append(li);
    retval=li->parse();
  } while (retval==RetVal_ListItem);
  
  if (retval==0)
  {
    printf("Error: unexpected end of comment at line %d while inside <%cl> block\n",
        doctokenizerYYlineno,m_type==Unordered ? 'u' : 'o');
  }

endlist:
  DBG(("DocHtmlList::parse() end retval=%x\n",retval));
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval==RetVal_EndList ? RetVal_OK : retval;
}

//---------------------------------------------------------------------------

int DocSimpleListItem::parse()
{
  g_nodeStack.push(this);
  int rv=m_paragraph->parse();
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return rv;
}

//--------------------------------------------------------------------------

int DocSimpleList::parse()
{
  g_nodeStack.push(this);
  int rv;
  do
  {
    DocSimpleListItem *li=new DocSimpleListItem(this);
    m_children.append(li);
    rv=li->parse();
  } while (rv==RetVal_ListItem);
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

//--------------------------------------------------------------------------

int DocAutoListItem::parse()
{
  int retval = RetVal_OK;
  g_nodeStack.push(this);
  retval=m_paragraph->parse();
  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

int DocAutoList::parse()
{
  int retval = RetVal_OK;
  g_nodeStack.push(this);
	  // first item or sub list => create new list
  do
  {
    DocAutoListItem *li = new DocAutoListItem(this);
    m_children.append(li);
    retval=li->parse();
  } 
  while (retval==TK_LISTITEM &&              // new list item
         m_indent==g_token->indent &&        // at same indent level
	 m_isEnumList==g_token->isEnumList   // of the same kind
        );

  DocNode *n=g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

void DocTitle::parse()
{
  DBG(("DocTitle::parse() start\n"));
  g_nodeStack.push(this);
  doctokenizerYYsetStateTitle();
  int tok;
  while ((tok=doctokenizerYYlex()))
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
          printf("Error: Illegal command %s as part of a title section at line %d\n",
	       g_token->name.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  doctokenizerYYsetStatePara();
  handlePendingStyleCommands(this,m_children);
  DBG(("DocTitle::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

//--------------------------------------------------------------------------

DocSimpleSect::DocSimpleSect(DocNode *parent,Type t) : 
          m_parent(parent), m_type(t) 
{ 
  m_paragraph = new DocPara(this); 
  //m_params.setAutoDelete(TRUE);
  m_title = 0;
}

DocSimpleSect::~DocSimpleSect() 
{ 
  delete m_paragraph; 
  delete m_title;
}

void DocSimpleSect::accept(DocVisitor *v)
{
  v->visitPre(this);
  if (m_title) m_title->accept(v);
  m_paragraph->accept(v);
  v->visitPost(this);
}

int DocSimpleSect::parse(bool userTitle)
{
  DBG(("DocSimpleSect::parse() start\n"));
  g_nodeStack.push(this);

  // handle case for user defined title
  if (userTitle)
  {
    m_title = new DocTitle(this);
    m_title->parse();
  }
  
  int retval = m_paragraph->parse();

  DBG(("DocSimpleSect::parse() end retval=%d\n",retval));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  return retval; // 0==EOF, TK_NEWPARA, TK_LISTITEM, TK_ENDLIST, RetVal_SimpleSec
}

void DocSimpleSect::addParam(const QCString &name) 
{ 
  m_params.append(name); 
}

//--------------------------------------------------------------------------

int DocPara::handleSimpleSection(DocSimpleSect::Type t)
{
  DocSimpleSect *ss=new DocSimpleSect(this,t);
  m_children.append(ss);
  int rv = ss->parse(t==DocSimpleSect::User);
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

int DocPara::handleParamSection(const QCString &cmdName,DocSimpleSect::Type t)
{
  int tok=doctokenizerYYlex();
  DocSimpleSect *ss=new DocSimpleSect(this,t);
  m_children.append(ss);
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
  }
  doctokenizerYYsetStateParam();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  while (tok==TK_WORD) /* there is a parameter name */
  {
    ss->addParam(g_token->name);
    tok=doctokenizerYYlex();
  }
  if (tok==0) /* premature end of comment block */
  {
    printf("Error: unexpected end of comment block at line %d while parsing the "
        "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
    return 0;
  }
  ASSERT(tok==TK_WHITESPACE);
  int rv = ss->parse(FALSE);
  return (rv!=TK_NEWPARA) ? rv : RetVal_OK;
}

#if 0
int DocPara::handleStyleArgument(const QCString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
	cmdName.data(),doctokenizerYYlineno);
    return;
  }
  while ((tok=doctokenizerYYlex()) && tok!=TK_WHITESPACE && tok!=TK_NEWPARA)
  {
    if (!defaultHandleToken(this,tok,m_children))
    {
      switch (tok)
      {
        case TK_COMMAND: 
	  printf("Error: Illegal command \\%s as the argument of a \\%s command at line %d\n",
	       g_token->name.data(),cmdName.data(),doctokenizerYYlineno);
          break;
        case TK_SYMBOL: 
	  printf("Error: Unsupported symbol %s found at line %d\n",
               g_token->name.data(),doctokenizerYYlineno);
          break;
        default:
	  printf("Error: Unexpected token %s at line %d\n",
		g_token->name.data(),doctokenizerYYlineno);
          break;
      }
    }
  }
  return tok==TK_NEWPARA ? TK_NEWPARA : RetVal_OK;
}

void DocPara::handleStyleEnter(DocStyleChange::Style s)
{
  DBG(("HandleStyleEnter\n"));
  DocStyleChange *sc= new DocStyleChange(this,g_nodeStack.count(),s,TRUE);
  m_children.append(sc);
  g_styleStack.push(sc);
}

void DocPara::handleStyleLeave(DocStyleChange::Style s,const char *tagName)
{
  DBG(("HandleStyleLeave\n"));
  if (g_styleStack.isEmpty() ||                           // no style change
      g_styleStack.top()->style()!=s ||                   // wrong style change
      g_styleStack.top()->position()!=g_nodeStack.count() // wrong position
     )
  {
    printf("Error: found </%s> tag at line %d without matching <%s> in the same paragraph\n",
        tagName,doctokenizerYYlineno,tagName);
  }
  else // end the section
  {
    DocStyleChange *sc= new DocStyleChange(this,g_nodeStack.count(),s,FALSE);
    m_children.append(sc);
    g_styleStack.pop();
  }
}
#endif

int DocPara::handleXRefItem(DocXRefItem::Type t)
{
  int retval=doctokenizerYYlex();
  ASSERT(retval==TK_WHITESPACE);
  doctokenizerYYsetStateXRefItem();
  retval=doctokenizerYYlex();
  if (retval!=0)
  {
    m_children.append(new DocXRefItem(this,g_token->id,t));
  }
  doctokenizerYYsetStatePara();
  return retval;
}

void DocPara::handleIncludeOperator(const QCString &cmdName,DocIncOperator::Type t)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStatePattern();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok==0)
  {
    printf("Error: unexpected end of comment block at line %d while parsing the "
        "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
    return;
  }
  else if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  DocIncOperator *op = new DocIncOperator(this,t,g_token->name);
  m_children.append(op);
}

void DocPara::handleImage(const QCString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  DocImage::Type t;
  QCString imgType = g_token->name.lower();
  if      (imgType=="html")  t=DocImage::Html;
  else if (imgType=="latex") t=DocImage::Latex;
  else if (imgType=="rtf")   t=DocImage::Rtf;
  else
  {
    printf("Error: image type %s specified as the first argument of "
        "%s at line %d is not valid.\n",
        imgType.data(),cmdName.data(),doctokenizerYYlineno);
    return;
  } 
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStatePara();
  DocImage *img = new DocImage(this,g_token->name,t);
  m_children.append(img);
  img->parse();
}

void DocPara::handleDotFile(const QCString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStatePara();
  DocDotFile *df = new DocDotFile(this,g_token->name);
  m_children.append(df);
  df->parse();
}

void DocPara::handleLink(const QCString &cmdName,bool isJavaLink)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStateLink();
  tok=doctokenizerYYlex();
  if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStatePara();
  DocLink *lnk = new DocLink(this,g_token->name);
  m_children.append(lnk);
  QCString leftOver = lnk->parse(isJavaLink);
  if (!leftOver.isEmpty())
  {
    m_children.append(new DocWord(this,leftOver));
  }
}

void DocPara::handleRef(const QCString &cmdName)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStateRef();
  tok=doctokenizerYYlex(); // get the reference id
  DocRef *ref=0;
  if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    goto endref;
  }
  ref = new DocRef(this,g_token->name);
  m_children.append(ref);
  ref->parse();
endref:
  doctokenizerYYsetStatePara();
}


int DocPara::handleLanguageSwitch()
{
  int retval=RetVal_OK;

  if (!insideLang(this)) // start a language section at this level
  {
    do
    {
      int tok = doctokenizerYYlex();
      if (tok==TK_WHITESPACE)
      {
        // end of language specific sections
        retval=RetVal_OK;
        goto endlang;
      }
      else if (tok==TK_NEWPARA)
      {
        // end of language specific sections
        retval = tok;
        goto endlang;
      }
      else if (tok==TK_WORD)
      {
        DocLanguage *dl = new DocLanguage(this,g_token->name);
        m_children.append(dl);
        retval = dl->parse();
      }
      else
      {
        printf("Error: Unexpected token %s as parameter of \\~ at line %d\n",
            tokToString(tok),doctokenizerYYlineno);
        goto endlang;
      }
    }
    while (retval==RetVal_SwitchLang);        
  }
  else // return from this section
  {
    retval = RetVal_SwitchLang;
  }
endlang:
  return retval;
}

void DocPara::handleInclude(const QCString &cmdName,DocInclude::Type t)
{
  int tok=doctokenizerYYlex();
  if (tok!=TK_WHITESPACE)
  {
    printf("Error: expected whitespace after %s command at line %d\n",
        cmdName.data(),doctokenizerYYlineno);
    return;
  }
  doctokenizerYYsetStateFile();
  tok=doctokenizerYYlex();
  doctokenizerYYsetStatePara();
  if (tok==0)
  {
    printf("Error: unexpected end of comment block at line %d while parsing the "
        "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
    return;
  }
  else if (tok!=TK_WORD)
  {
    printf("Error: unexpected token %s as the argument of %s at line %d.\n",
        tokToString(tok),cmdName.data(),doctokenizerYYlineno);
    return;
  }
  DocInclude *inc = new DocInclude(this,g_token->name,t);
  m_children.append(inc);
}


int DocPara::handleCommand(const QCString &cmdName)
{
  int retval = RetVal_OK;
  switch (CmdMapper::map(cmdName))
  {
    case CMD_UNKNOWN:
      printf("Error: Found unknown command `\\%s' at line %d\n",cmdName.data(),doctokenizerYYlineno);
      break;
    case CMD_EMPHASIS:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Italic,FALSE));
      break;
    case CMD_BOLD:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Bold,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Bold,FALSE));
      break;
    case CMD_CODE:
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Code,TRUE));
      retval=handleStyleArgument(this,m_children,cmdName); 
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),DocStyleChange::Code,FALSE));
      break;
    case CMD_BSLASH:
      m_children.append(new DocSymbol(this,DocSymbol::BSlash));
      break;
    case CMD_AT:
      m_children.append(new DocSymbol(this,DocSymbol::At));
      break;
    case CMD_LESS:
      m_children.append(new DocSymbol(this,DocSymbol::Less));
      break;
    case CMD_GREATER:
      m_children.append(new DocSymbol(this,DocSymbol::Greater));
      break;
    case CMD_AMP:
      m_children.append(new DocSymbol(this,DocSymbol::Amp));
      break;
    case CMD_DOLLAR:
      m_children.append(new DocSymbol(this,DocSymbol::Dollar));
      break;
    case CMD_HASH:
      m_children.append(new DocSymbol(this,DocSymbol::Hash));
      break;
    case CMD_PERCENT:
      m_children.append(new DocSymbol(this,DocSymbol::Percent));
      break;
    case CMD_SA:
      retval = handleSimpleSection(DocSimpleSect::See);
      break;
    case CMD_RETURN:
      retval = handleSimpleSection(DocSimpleSect::Return);
      break;
    case CMD_AUTHOR:
      retval = handleSimpleSection(DocSimpleSect::Author);
      break;
    case CMD_VERSION:
      retval = handleSimpleSection(DocSimpleSect::Version);
      break;
    case CMD_SINCE:
      retval = handleSimpleSection(DocSimpleSect::Since);
      break;
    case CMD_DATE:
      retval = handleSimpleSection(DocSimpleSect::Date);
      break;
    case CMD_NOTE:
      retval = handleSimpleSection(DocSimpleSect::Note);
      break;
    case CMD_WARNING:
      retval = handleSimpleSection(DocSimpleSect::Warning);
      break;
    case CMD_PRE:
      retval = handleSimpleSection(DocSimpleSect::Pre);
      break;
    case CMD_POST:
      retval = handleSimpleSection(DocSimpleSect::Post);
      break;
    case CMD_INVARIANT:
      retval = handleSimpleSection(DocSimpleSect::Invar);
      break;
    case CMD_REMARK:
      retval = handleSimpleSection(DocSimpleSect::Remark);
      break;
    case CMD_ATTENTION:
      retval = handleSimpleSection(DocSimpleSect::Attention);
      break;
    case CMD_PAR:
      retval = handleSimpleSection(DocSimpleSect::User);
      break;
    case CMD_LI:
      {
	DocSimpleList *sl=new DocSimpleList(this);
	m_children.append(sl);
        retval = sl->parse();
      }
      break;
    case CMD_SECTION:
      {
	// get the argument of the section command.
	int tok=doctokenizerYYlex();
	if (tok!=TK_WHITESPACE)
	{
	  printf("Error: expected whitespace after %s command at line %d\n",
	      cmdName.data(),doctokenizerYYlineno);
	  break;
	}
	tok=doctokenizerYYlex();
	if (tok==0)
	{
	  printf("Error: unexpected end of comment block at line %d while parsing the "
	      "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
	  break;
	}
	else if (tok!=TK_WORD)
	{
	  printf("Error: unexpected token %s as the argument of %s at line %d.\n",
	      tokToString(tok),cmdName.data(),doctokenizerYYlineno);
	  break;
	}
	g_token->sectionId = g_token->name;
	retval = RetVal_Section;
      }
      break;
    case CMD_STARTCODE:
      {
        doctokenizerYYsetStateCode();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_token->verb,DocVerbatim::Code));
        if (retval==0) printf("Error: code section ended without end marker at line %d\n",
            doctokenizerYYlineno);
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_HTMLONLY:
      {
        doctokenizerYYsetStateHtmlOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_token->verb,DocVerbatim::HtmlOnly));
        if (retval==0) printf("Error: htmlonly section ended without end marker at line %d\n",
            doctokenizerYYlineno);
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_LATEXONLY:
      {
        doctokenizerYYsetStateLatexOnly();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_token->verb,DocVerbatim::LatexOnly));
        if (retval==0) printf("Error: latexonly section ended without end marker at line %d\n",
            doctokenizerYYlineno);
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_VERBATIM:
      {
        doctokenizerYYsetStateVerbatim();
        retval = doctokenizerYYlex();
        m_children.append(new DocVerbatim(this,g_token->verb,DocVerbatim::Verbatim));
        if (retval==0) printf("Error: verbatim section ended without end marker at line %d\n",
            doctokenizerYYlineno);
        doctokenizerYYsetStatePara();
      }
      break;
    case CMD_ENDCODE:
    case CMD_ENDHTMLONLY:
    case CMD_ENDLATEXONLY:
    case CMD_ENDLINK:
    case CMD_ENDVERBATIM:
      printf("Error: unexpected command %s at line %d\n",g_token->name.data(),doctokenizerYYlineno);
      break; 
    case CMD_PARAM:
      retval = handleParamSection(cmdName,DocSimpleSect::Param);
      break;
    case CMD_RETVAL:
      retval = handleParamSection(cmdName,DocSimpleSect::RetVal);
      break;
    case CMD_EXCEPTION:
      retval = handleParamSection(cmdName,DocSimpleSect::Exception);
      break;
    case CMD_BUG:
      retval = handleXRefItem(DocXRefItem::Bug);
      break;
    case CMD_TODO:
      retval = handleXRefItem(DocXRefItem::Todo);
      break;
    case CMD_TEST:
      retval = handleXRefItem(DocXRefItem::Test);
      break;
    case CMD_DEPRECATED:
      retval = handleXRefItem(DocXRefItem::Deprecated);
      break;
    case CMD_LINEBREAK:
      {
        DocLineBreak *lb = new DocLineBreak(this);
        m_children.append(lb);
      }
      break;
    case CMD_ANCHOR:
      {
	int tok=doctokenizerYYlex();
	if (tok!=TK_WHITESPACE)
	{
	  printf("Error: expected whitespace after %s command at line %d\n",
	      cmdName.data(),doctokenizerYYlineno);
	  break;
	}
	tok=doctokenizerYYlex();
	if (tok==0)
	{
	  printf("Error: unexpected end of comment block at line %d while parsing the "
	      "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
	  break;
	}
	else if (tok!=TK_WORD)
	{
	  printf("Error: unexpected token %s as the argument of %s at line %d.\n",
	      tokToString(tok),cmdName.data(),doctokenizerYYlineno);
	  break;
	}
        DocAnchor *anchor = new DocAnchor(this,g_token->name);
        m_children.append(anchor);
      }
      break;
    case CMD_ADDINDEX:
      {
        DocIndexEntry *ie = new DocIndexEntry(this);
        m_children.append(ie);
        retval = ie->parse();
      }
      break;
    case CMD_INTERNAL:
      retval = RetVal_Internal;
      break;
    case CMD_COPYDOC:
      {
	int tok=doctokenizerYYlex();
	if (tok!=TK_WHITESPACE)
	{
	  printf("Error: expected whitespace after %s command at line %d\n",
	      cmdName.data(),doctokenizerYYlineno);
	  break;
	}
	tok=doctokenizerYYlex();
	if (tok==0)
	{
	  printf("Error: unexpected end of comment block at line %d while parsing the "
	      "argument of command %s\n",doctokenizerYYlineno, cmdName.data());
	  break;
	}
	else if (tok!=TK_WORD)
	{
	  printf("Error: unexpected token %s as the argument of %s at line %d.\n",
	      tokToString(tok),cmdName.data(),doctokenizerYYlineno);
	  break;
	}
        DocCopy *cpy = new DocCopy(this,g_token->name);
        m_children.append(cpy);
      }
      break;
    case CMD_INCLUDE:
      handleInclude(cmdName,DocInclude::Include);
      break;
    case CMD_DONTINCLUDE:
      handleInclude(cmdName,DocInclude::DontInclude);
      break;
    case CMD_HTMLINCLUDE:
      handleInclude(cmdName,DocInclude::HtmlInclude);
      break;
    case CMD_VERBINCLUDE:
      handleInclude(cmdName,DocInclude::VerbInclude);
      break;
    case CMD_SKIP:
      handleIncludeOperator(cmdName,DocIncOperator::Skip);
      break;
    case CMD_UNTIL:
      handleIncludeOperator(cmdName,DocIncOperator::Until);
      break;
    case CMD_SKIPLINE:
      handleIncludeOperator(cmdName,DocIncOperator::SkipLine);
      break;
    case CMD_LINE:
      handleIncludeOperator(cmdName,DocIncOperator::Line);
      break;
    case CMD_IMAGE:
      handleImage(cmdName);
      break;
    case CMD_DOTFILE:
      handleDotFile(cmdName);
      break;
    case CMD_LINK:
      handleLink(cmdName,FALSE);
      break;
    case CMD_JAVALINK:
      handleLink(cmdName,TRUE);
      break;
    case CMD_REF:
      handleRef(cmdName);
      break;
    case CMD_SECREFLIST:
      {
        DocSecRefList *list = new DocSecRefList(this);
        m_children.append(list);
        list->parse();
      }
      break;
    case CMD_SECREFITEM:
      printf("Error: unexpected command %s at line %d\n",g_token->name.data(),doctokenizerYYlineno);
      break;
    case CMD_ENDSECREFLIST:
      printf("Error: unexpected command %s at line %d\n",g_token->name.data(),doctokenizerYYlineno);
      break;
    case CMD_FORMULA:
      {
        DocFormula *form=new DocFormula(this,g_token->id);
        m_children.append(form);
      }
      break;
    case CMD_LANGSWITCH:
      retval = handleLanguageSwitch();
      break;
    default:
      // we should not get here!
      ASSERT(0);
      break;
  }
  ASSERT(retval==0 || retval==RetVal_OK || retval==RetVal_SimpleSec || 
         retval==TK_LISTITEM || retval==TK_ENDLIST || retval==TK_NEWPARA ||
         retval==RetVal_Section || retval==RetVal_EndList || 
         retval==RetVal_Internal || retval==RetVal_SwitchLang
        );
  return retval;
}

#if 0
void DocPara::handlePendingStyleCommands()
{
  if (!g_styleStack.isEmpty())
  {
    DocStyleChange *sc = g_styleStack.top();
    while (sc && sc->position()>=g_nodeStack.count()) 
    { // there are unclosed style modifiers in the paragraph
      const char *cmd;
      switch (sc->style())
      {
        case DocStyleChange::Bold:        cmd = "b"; break;
        case DocStyleChange::Italic:      cmd = "em"; break;
        case DocStyleChange::Code:        cmd = "code"; break;
        case DocStyleChange::Center:      cmd = "center"; break;
        case DocStyleChange::Small:       cmd = "small"; break;
        case DocStyleChange::Subscript:   cmd = "subscript"; break;
        case DocStyleChange::Superscript: cmd = "superscript"; break;
      }
      printf("Error: end of paragraph at line %d without end of style "
             "command </%s>\n",doctokenizerYYlineno,cmd);
      m_children.append(new DocStyleChange(this,g_nodeStack.count(),sc->style(),FALSE));
      g_styleStack.pop();
      sc = g_styleStack.top();
    }
  }
}
#endif

int DocPara::handleHtmlStartTag(const QCString &tagName,const QList<Option> &tagOptions)
{
  DBG(("handleHtmlStartTag(%s,%d)\n",tagName.data(),tagOptions.count()));
  int retval=RetVal_OK;
  int tagId = HtmlTagMapper::map(tagName);
  switch (tagId)
  {
    case HTML_UL: 
      {
        DocHtmlList *list = new DocHtmlList(this,DocHtmlList::Unordered);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_OL: 
      {
        DocHtmlList *list = new DocHtmlList(this,DocHtmlList::Ordered);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_LI:
      if (!insideUL(this) && !insideOL(this))
      {
        printf("Error: lonely <li> tag found at line %d\n",doctokenizerYYlineno);
      }
      else
      {
        retval=RetVal_ListItem;
      }
      break;
    case HTML_PRE:
      {
        DocHtmlPre *pre = new DocHtmlPre(this);
        m_children.append(pre);
        retval=pre->parse();
      }
      break;
    case HTML_BOLD:
      handleStyleEnter(this,m_children,DocStyleChange::Bold);
      break;
    case HTML_CODE:
      handleStyleEnter(this,m_children,DocStyleChange::Code);
      break;
    case HTML_EMPHASIS:
      handleStyleEnter(this,m_children,DocStyleChange::Italic);
      break;
    case HTML_SUB:
      handleStyleEnter(this,m_children,DocStyleChange::Subscript);
      break;
    case HTML_SUP:
      handleStyleEnter(this,m_children,DocStyleChange::Superscript);
      break;
    case HTML_CENTER:
      handleStyleEnter(this,m_children,DocStyleChange::Center);
      break;
    case HTML_SMALL:
      handleStyleEnter(this,m_children,DocStyleChange::Small);
      break;
    case HTML_P:
      retval=TK_NEWPARA;
      break;
    case HTML_DL:
      {
        DocHtmlDescList *list = new DocHtmlDescList(this);
        m_children.append(list);
        retval=list->parse();
      }
      break;
    case HTML_DT:
      retval = RetVal_DescTitle;
      break;
    case HTML_DD:
      printf("Error: Unexpected tag <dd> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_TABLE:
      {
        DocHtmlTable *table = new DocHtmlTable(this);
        m_children.append(table);
        retval=table->parse();
      }
      break;
    case HTML_TR:
      retval = RetVal_TableRow;
      break;
    case HTML_TD:
      retval = RetVal_TableCell;
      break;
    case HTML_TH:
      retval = RetVal_TableHCell;
      break;
    case HTML_CAPTION:
      printf("Error: Unexpected tag <caption> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_BR:
      {
        DocLineBreak *lb = new DocLineBreak(this);
        m_children.append(lb);
      }
      break;
    case HTML_HR:
      {
        DocHorRuler *hr = new DocHorRuler(this);
        m_children.append(hr);
      }
      break;
    case HTML_A:
      {
        QListIterator<Option> li(tagOptions);
        Option *opt;
        for (li.toFirst();(opt=li.current());++li)
        {
          if (opt->name=="name") // <a name=label> tag
          {
            if (!opt->value.isEmpty())
            {
              DocAnchor *anc = new DocAnchor(this,opt->value);
              m_children.append(anc);
              break; // stop looking for other tag options
            }
            else
            {
              printf("Error: found <a> tag at line %d with name option but without value!\n",
                  doctokenizerYYlineno);
            }
          }
          else if (opt->name=="href") // <a href=url>..</a> tag
          {
            DocHRef *href = new DocHRef(this,opt->value);
            m_children.append(href);
            retval = href->parse();
            break;
          }
          else // unsupport option for tag a
          {
          }
        }
      }
      break;
    case HTML_H1:
      {
        DocHtmlHeader *header = new DocHtmlHeader(this,1);
        m_children.append(header);
        retval = header->parse();
      }
      break;
    case HTML_H2:
      {
        DocHtmlHeader *header = new DocHtmlHeader(this,2);
        m_children.append(header);
        retval = header->parse();
      }
      break;
    case HTML_H3:
      {
        DocHtmlHeader *header = new DocHtmlHeader(this,3);
        m_children.append(header);
        retval = header->parse();
      }
      break;
    case HTML_IMG:
      {
        QListIterator<Option> li(tagOptions);
        Option *opt;
        for (li.toFirst();(opt=li.current());++li)
        {
          if (opt->name=="src" && !opt->value.isEmpty())
          {
            DocImage *img = new DocImage(this,opt->value,DocImage::Html);
            m_children.append(img);
          }
        }
      }
      break;
    case HTML_UNKNOWN:
      printf("Error: Unsupported html tag <%s> found at line %d\n",
          tagName.data(), doctokenizerYYlineno);
      break;
    default:
      // we should not get here!
      ASSERT(0);
      break;
  }
  return retval;
}

int DocPara::handleHtmlEndTag(const QCString &tagName)
{
  DBG(("handleHtmlEndTag(%s)\n",tagName.data()));
  int tagId = HtmlTagMapper::map(tagName);
  int retval=RetVal_OK;
  switch (tagId)
  {
    case HTML_UL: 
      if (!insideUL(this))
      {
        printf("Error: found </ul> tag at line %d without matching <ul>\n",
            doctokenizerYYlineno);
      }
      else
      {
        retval=RetVal_EndList;
      }
      break;
    case HTML_OL: 
      if (!insideOL(this))
      {
        printf("Error: found </ol> tag at line %d without matching <ol>\n",
            doctokenizerYYlineno);
      }
      else
      {
        retval=RetVal_EndList;
      }
      break;
    case HTML_LI:
      if (!insideLI(this))
      {
        printf("Error: found </li> tag at line %d without matching <li>\n",
            doctokenizerYYlineno);
      }
      else
      {
        // ignore </li> tags
      }
      break;
    case HTML_PRE:
      if (!insidePRE(this))
      {
        printf("Error: found </pre> tag at line %d without matching <pre>\n",
            doctokenizerYYlineno);
      }
      else
      {
        retval=RetVal_EndPre;
      }
      break;
    case HTML_BOLD:
      handleStyleLeave(this,m_children,DocStyleChange::Bold,"b");
      break;
    case HTML_CODE:
      handleStyleLeave(this,m_children,DocStyleChange::Code,"code");
      break;
    case HTML_EMPHASIS:
      handleStyleLeave(this,m_children,DocStyleChange::Italic,"em");
      break;
    case HTML_SUB:
      handleStyleLeave(this,m_children,DocStyleChange::Subscript,"sub");
      break;
    case HTML_SUP:
      handleStyleLeave(this,m_children,DocStyleChange::Superscript,"sup");
      break;
    case HTML_CENTER:
      handleStyleLeave(this,m_children,DocStyleChange::Center,"center");
      break;
    case HTML_SMALL:
      handleStyleLeave(this,m_children,DocStyleChange::Small,"small");
      break;
    case HTML_P:
      // ignore </p> tag
      break;
    case HTML_DL:
      retval=RetVal_EndDesc;
      break;
    case HTML_DT:
      // ignore </dt> tag
      break;
    case HTML_DD:
      // ignore </dd> tag
      break;
    case HTML_TABLE:
      retval=RetVal_EndTable;
      break;
    case HTML_TR:
      // ignore </tr> tag
      break;
    case HTML_TD:
      // ignore </td> tag
      break;
    case HTML_TH:
      // ignore </th> tag
      break;
    case HTML_CAPTION:
      printf("Error: Unexpected tag </caption> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_BR:
      printf("Error: Illegal </br> tag found\n");
      break;
    case HTML_H1:
      printf("Error: Unexpected tag </h1> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_H2:
      printf("Error: Unexpected tag </h2> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_H3:
      printf("Error: Unexpected tag </h3> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_IMG:
      printf("Error: Unexpected tag </img> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_HR:
      printf("Error: Unexpected tag </hr> found at line %d\n",
          doctokenizerYYlineno);
      break;
    case HTML_A:
      //printf("Error: Unexpected tag </a> found at line %d\n",
      //    doctokenizerYYlineno);
      // ignore </a> tag (can be part of <a name=...></a>
      break;
    case HTML_UNKNOWN:
      printf("Error: Unsupported html tag </%s> found at line %d\n",
          tagName.data(), doctokenizerYYlineno);
      break;
    default:
      // we should not get here!
      ASSERT(0);
      break;
  }
  return retval;
}

int DocPara::parse()
{
  DBG(("DocPara::parse() start\n"));
  g_nodeStack.push(this);
  int tok;
  int retval=0;
  while ((tok=doctokenizerYYlex())) // get the next token
  {
reparsetoken:
    DBG(("token %s at %d",tokToString(tok),doctokenizerYYlineno));
    if (tok==TK_WORD || tok==TK_SYMBOL || tok==TK_URL || 
        tok==TK_COMMAND || tok==TK_HTMLTAG
       )
    {
      DBG((" name=%s",g_token->name.data()));
    }
    DBG(("\n"));
    switch(tok)
    {
      case TK_WORD:        
	m_children.append(new DocWord(this,g_token->name));
	break;
      case TK_URL:
        m_children.append(new DocURL(this,g_token->name));
        break;
      case TK_WHITESPACE:  
	// prevent leading whitespace and collapse multiple whitespace areas
	if (insidePRE(this) || // all whitespace is relavant
            (                  // keep only whitespace after words, URL or symbols
              !m_children.isEmpty() && 
              (
	         m_children.last()->kind()==DocNode::Kind_Word   ||
	         m_children.last()->kind()==DocNode::Kind_URL    ||
	         m_children.last()->kind()==DocNode::Kind_Symbol
               )
	     )
           )
	{
	  m_children.append(new DocWhiteSpace(this,g_token->chars));
	}
	break;
      case TK_LISTITEM:    
	{
	  DBG(("found list item at %d parent=%d\n",g_token->indent,parent()->kind()));
	  DocNode *n=parent();
	  while (n && n->kind()!=DocNode::Kind_AutoList) n=n->parent();
	  if (n) // we found an auto list up in the hierarchy
	  {
	    DocAutoList *al = (DocAutoList *)n;
	    DBG(("previous list item at %d\n",al->indent()));
	    if (al->indent()>=g_token->indent) 
	      // new item at the same or lower indent level
	    {
	      retval=TK_LISTITEM;
	      goto endparagraph;
	    }
	  }
	  // first item or sub list => create new list
	  DocAutoList *al=0;
	  do
	  {
	    al = new DocAutoList(this,g_token->indent,g_token->isEnumList);
	    m_children.append(al);
	    retval = al->parse();
	  } while (retval==TK_LISTITEM &&         // new list
	           al->indent()==g_token->indent  // at some indent level
		  );
	      
	  // check the return value
	  if (retval==RetVal_SimpleSec) // auto list ended due to simple section command
	  {
	    // Reparse the token that ended the section at this level,
	    // so a new simple section will be started at this level.
	    // This is the same as unputting the last read token and continuing.
	    g_token->name = g_token->simpleSectName;
	    tok = TK_COMMAND;
	    DBG(("reparsing command %s\n",g_token->name.data()));
	    goto reparsetoken;
	  }
	  else if (retval==TK_ENDLIST)
	  {
	    if (al->indent()>g_token->indent) // end list
	    {
	      goto endparagraph;
	    }
	    else // continue with current paragraph
	    {
	    }
	  }
	  else // paragraph ended due to TK_NEWPARA, TK_LISTITEM, or EOF
	  {
	    goto endparagraph;
	  }
	}
	break;
      case TK_ENDLIST:     
	DBG(("Found end of list inside of paragraph at line %d\n",doctokenizerYYlineno));
	if (parent()->kind()==DocNode::Kind_AutoListItem)
	{
	  ASSERT(parent()->parent()->kind()==DocNode::Kind_AutoList);
	  DocAutoList *al = (DocAutoList *)parent()->parent();
	  if (al->indent()>=g_token->indent)
	  {
	    // end of list marker ends this paragraph
	    retval=TK_ENDLIST;
	    goto endparagraph;
	  }
	  else
	  {
	    printf("Error: End of list marker found at line %d "
		   "has invalid indent level ",doctokenizerYYlineno);
	  }
	}
	else
	{
	  printf("Error: End of list marker found at line %d without any preceding "
	         "list items\n",doctokenizerYYlineno);
	}
	break;
      case TK_COMMAND:    
	{
	  // see if we have to start a simple section
	  int cmd = CmdMapper::map(g_token->name);
	  DocNode *n=parent();
	  while (n && n->kind()!=DocNode::Kind_SimpleSect) n=n->parent();
	  if (cmd&SIMPLESECT_BIT)
	  {
	    if (n  // already in a simple section
	        //|| // no section or root as parent
		//  (parent()->kind()!=DocNode::Kind_Root &&
	  	//   parent()->kind()!=DocNode::Kind_Section
	        //  )
	       )
	    {
	      // simple section cannot start in this paragraph, need
	      // to unwind the stack and remember the command.
	      g_token->simpleSectName = g_token->name.copy();
	      retval=RetVal_SimpleSec;
	      goto endparagraph;
	    }
	  }
	  // see if we are in a simple list
	  n=parent();
	  while (n && n->kind()!=DocNode::Kind_SimpleListItem) n=n->parent();
	  if (n)
	  {
	    if (cmd==CMD_LI)
	    {
	      retval=RetVal_ListItem;
	      goto endparagraph;
	    }
	  }
	  
	  // handle the command
	  retval=handleCommand(g_token->name.copy());

	  // check the return value
	  if (retval==RetVal_SimpleSec)
	  {
	    // Reparse the token that ended the section at this level,
	    // so a new simple section will be started at this level.
	    // This is the same as unputting the last read token and continuing.
	    g_token->name = g_token->simpleSectName;
	    tok = TK_COMMAND;
	    DBG(("reparsing command %s\n",g_token->name.data()));
	    goto reparsetoken;
	  }
	  else if (retval==RetVal_OK) 
	  {
	    // the command ended normally, keep scanner for new tokens.
	    retval = 0;
	  }
	  else // end of file, end of paragraph, start or end of section 
	       // or some auto list marker
	  {
	    goto endparagraph;
	  }
	}
	break;
      case TK_HTMLTAG:    
        {
          if (!g_token->endTag) // found a start tag
          {
            retval = handleHtmlStartTag(g_token->name,g_token->options);
          }
          else // found an end tag
          {
            retval = handleHtmlEndTag(g_token->name);
          }
          if (retval==RetVal_OK) 
          {
	    // the command ended normally, keep scanner for new tokens.
            retval = 0;
          }
          else
          {
            goto endparagraph;
          }
        }
	break;
      case TK_SYMBOL:     
	break;
      case TK_NEWPARA:     
	retval=TK_NEWPARA;
	goto endparagraph;
    }
  }
endparagraph:
  handlePendingStyleCommands(this,m_children);
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  DBG(("DocPara::parse() end retval=%x\n",retval));
  ASSERT(retval==0 || retval==TK_NEWPARA || retval==TK_LISTITEM || 
         retval==TK_ENDLIST || retval>RetVal_OK 
	);

  if (!(retval==0 || retval==TK_NEWPARA || retval==TK_LISTITEM || 
         retval==TK_ENDLIST || retval>RetVal_OK 
	)) printf("DocPara::parse: Error retval=%x unexpected at line %d!\n",retval,doctokenizerYYlineno);
  return retval; 
}

//--------------------------------------------------------------------------

int DocSection::parse()
{
  DBG(("DocSection::parse() start %s\n",g_token->sectionId.data()));
  int retval=RetVal_OK;
  g_nodeStack.push(this);

  // first parse any number of paragraphs
  do
  {
    DocPara *par = new DocPara(this);
    retval=par->parse();
    if (!par->isEmpty()) m_children.append(par);
    if (retval==TK_LISTITEM)
    {
      printf("Error: Invalid list item found at line %d!\n",doctokenizerYYlineno);
    }
  } while (retval!=0 && retval!=RetVal_Section && retval!=RetVal_Internal);

  // then parse any number of nested sections
  while (retval==RetVal_Section) // more sections follow
  {
    SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
    int secLev = sec->type==SectionInfo::Subsection ? 2 : 1;
    if (secLev==level()) // new section at same level 
    {
      break;
    }
    if (secLev!=level()+1) // new section at wrong level
    {
      printf("Error: Expected level %d section, found a section "
	  "with level %d at line %d.\n",level()+1,secLev,doctokenizerYYlineno);
      retval=0; // stop parsing any further.
      break;
    }
    else // nested section
    {
      DocSection *s=new DocSection(this,secLev,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }
  ASSERT(retval==0 || retval==RetVal_Section);

  DBG(("DocSection::parse() end\n"));
  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
  return retval;
}

//--------------------------------------------------------------------------

void DocRoot::parse()
{
  g_nodeStack.push(this);
  doctokenizerYYsetStatePara();
  DocPara *par=0;
  int retval=0;

  // first parse any number of paragraphs
  do
  {
    par = new DocPara(this);
    retval=par->parse();
    if (!par->isEmpty()) m_children.append(par);
    if (retval==TK_LISTITEM)
    {
      printf("Error: Invalid list item found at line %d!\n",doctokenizerYYlineno);
    }
  } while (retval!=0 && retval!=RetVal_Section && retval!=RetVal_Internal);

  // then parse any number of level1 sections
  while (retval==RetVal_Section)
  {
    SectionInfo *sec=Doxygen::sectionDict[g_token->sectionId];
    int secLev = sec->type==SectionInfo::Subsection ? 2 : 1;
    if (secLev!=1) // wrong level
    {
      printf("Error: Expected level 1 section, found a section with level %d at line %d.\n",secLev,doctokenizerYYlineno);
      break;
    }
    else
    {
      DocSection *s=new DocSection(this,secLev,g_token->sectionId);
      m_children.append(s);
      retval = s->parse();
    }
  }

  if (retval==RetVal_Internal)
  {
    DocInternal *in = new DocInternal(this);
    m_children.append(in);
    retval = in->parse();
  }

  DocNode *n = g_nodeStack.pop();
  ASSERT(n==this);
}

//--------------------------------------------------------------------------

void validatingParseDoc(const char *fileName,int startLine,const char *input)
{
  //printf("---------------- input --------------------\n%s\n----------- end input -------------------\n",input);
  //
  printf("========== validating %s at line %d\n",fileName,startLine);
  g_token = new TokenInfo;
  
  doctokenizerYYlineno=startLine;
  doctokenizerYYinit(input);

  // build abstract syntax tree
  DocRoot *root = new DocRoot;
  root->parse();

  if (Debug::isFlagSet(Debug::PrintTree))
  {
    // pretty print the result
    PrintDocVisitor *v = new PrintDocVisitor;
    root->accept(v);
  }

  delete root;

  delete g_token;

  // TODO: These should be called at the end of the program.
  //doctokenizerYYcleanup();
  //CmdMapper::freeInstance();
  //HtmlTagMapper::freeInstance();
}
